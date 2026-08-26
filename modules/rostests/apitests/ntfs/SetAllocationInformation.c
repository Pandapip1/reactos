/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for FileAllocationInformation on a writable NTFS volume
 * COPYRIGHT:   Copyright 2026 Gavin John <gavinnjohn@gmail.com>
 *
 * Setting FileAllocationInformation must never destroy file contents. The rule,
 * measured on Windows 11 Pro 22621 with 4096-byte clusters, is: round the
 * requested allocation up to the cluster size, then truncate the file only if
 * the rounded value is below the current end of file. The end of file is never
 * extended.
 *
 * Before the NTFS fix this test targets, the driver fell FileAllocationInformation
 * through to FileEndOfFileInformation, so a request of 100 bytes against a
 * 4096-byte file truncated it to 100 bytes and destroyed 3996 bytes of user data.
 *
 * Note that a cluster-aligned request (case 3 below) behaves identically before
 * and after the fix, so it cannot discriminate on its own; cases 1 and 2 are the
 * ones that fail against the old behaviour.
 *
 * This is a standalone console program rather than a wine-style apitest because
 * it needs a writable NTFS volume, which requires the non-default registry value
 * MyDataDoesNotMatterSoEnableExperimentalWriteSupportForEveryNTFSVolume under the
 * NTFS driver's service key. Run it as:
 *
 *     ntfs_allocinfo_test.exe D:\
 *
 * with a directory on a writable NTFS volume. It also runs unchanged on Windows,
 * where it documents the reference behaviour.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_NO_STATUS
#include <windows.h>

#define NTOS_MODE_USER
#include <ndk/iofuncs.h>
#include <ndk/rtlfuncs.h>

static ULONG g_CheckNumber = 0;
static ULONG g_Executed = 0;
static ULONG g_Passed = 0;
static ULONG g_Failed = 0;

static
void
Check(
    const char *Name,
    ULONGLONG Expected,
    ULONGLONG Actual)
{
    BOOL Ok = (Expected == Actual);

    ++g_CheckNumber;
    ++g_Executed;
    if (Ok)
        ++g_Passed;
    else
        ++g_Failed;

    printf("CHECK %02lu [%s] %-36s exp=%I64u act=%I64u\n",
           g_CheckNumber, Ok ? "PASS" : "FAIL", Name, Expected, Actual);
}

static
void
Skip(
    const char *Name,
    const char *Reason)
{
    ++g_CheckNumber;
    printf("CHECK %02lu [SKIP] %-36s %s\n", g_CheckNumber, Name, Reason);
}

/* Deterministic, position-dependent pattern, so that a shifted or zero-filled
 * buffer cannot pass a content check by accident. */
static
UCHAR
PatternByte(
    ULONGLONG Offset)
{
    return (UCHAR)(((Offset * 31) ^ (Offset >> 8)) & 0xFF);
}

static
BOOL
WritePattern(
    HANDLE Handle,
    ULONG Size)
{
    UCHAR *Buffer;
    DWORD Written;
    ULONG i;
    BOOL Ok;

    if (Size == 0)
        return TRUE;

    Buffer = malloc(Size);
    if (Buffer == NULL)
        return FALSE;

    for (i = 0; i < Size; i++)
        Buffer[i] = PatternByte(i);

    Ok = WriteFile(Handle, Buffer, Size, &Written, NULL) && Written == Size;
    free(Buffer);

    if (Ok)
        Ok = FlushFileBuffers(Handle);

    return Ok;
}

/* Returns the number of leading bytes that still hold their pattern value, or
 * -1 if the file could not be read back at all. */
static
LONGLONG
CountGoodBytes(
    const WCHAR *Path,
    ULONGLONG Size)
{
    HANDLE Handle;
    UCHAR *Buffer;
    DWORD Read;
    ULONGLONG i;
    LONGLONG Good;

    if (Size == 0)
        return 0;

    Handle = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return -1;

    Buffer = malloc((SIZE_T)Size);
    if (Buffer == NULL)
    {
        CloseHandle(Handle);
        return -1;
    }

    if (!ReadFile(Handle, Buffer, (DWORD)Size, &Read, NULL) || Read != Size)
    {
        free(Buffer);
        CloseHandle(Handle);
        return -1;
    }

    Good = 0;
    for (i = 0; i < Size; i++)
    {
        if (Buffer[i] != PatternByte(i))
            break;
        ++Good;
    }

    free(Buffer);
    CloseHandle(Handle);
    return Good;
}

static
void
RunCase(
    const WCHAR *Path,
    ULONG ClusterSize,
    const char *CaseName,
    ULONG InitialSize,
    ULONGLONG RequestedAllocation)
{
    char Name[80];
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    FILE_ALLOCATION_INFORMATION AllocInfo;
    FILE_STANDARD_INFORMATION StandardInfo;
    ULONGLONG Rounded;
    ULONGLONG ExpectedEndOfFile;
    ULONGLONG ActualEndOfFile;
    LONGLONG GoodBytes;

    Rounded = (RequestedAllocation + ClusterSize - 1) & ~((ULONGLONG)ClusterSize - 1);
    ExpectedEndOfFile = (Rounded < InitialSize) ? Rounded : InitialSize;

    DeleteFileW(Path);
    Handle = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE || !WritePattern(Handle, InitialSize))
    {
        if (Handle != INVALID_HANDLE_VALUE)
            CloseHandle(Handle);
        sprintf(Name, "%s.status", CaseName);
        Skip(Name, "could not create the test file");
        sprintf(Name, "%s.eof", CaseName);
        Skip(Name, "could not create the test file");
        sprintf(Name, "%s.content", CaseName);
        Skip(Name, "could not create the test file");
        return;
    }

    AllocInfo.AllocationSize.QuadPart = (LONGLONG)RequestedAllocation;
    Status = NtSetInformationFile(Handle, &IoStatus, &AllocInfo, sizeof(AllocInfo),
                                  FileAllocationInformation);

    sprintf(Name, "%s.status", CaseName);
    Check(Name, (ULONGLONG)(ULONG)STATUS_SUCCESS, (ULONGLONG)(ULONG)Status);

    StandardInfo.EndOfFile.QuadPart = -1;
    if (!NT_SUCCESS(NtQueryInformationFile(Handle, &IoStatus, &StandardInfo,
                                           sizeof(StandardInfo),
                                           FileStandardInformation)))
    {
        CloseHandle(Handle);
        sprintf(Name, "%s.eof", CaseName);
        Skip(Name, "FileStandardInformation query failed");
        sprintf(Name, "%s.content", CaseName);
        Skip(Name, "FileStandardInformation query failed");
        return;
    }

    ActualEndOfFile = (ULONGLONG)StandardInfo.EndOfFile.QuadPart;
    CloseHandle(Handle);

    sprintf(Name, "%s.eof", CaseName);
    Check(Name, ExpectedEndOfFile, ActualEndOfFile);

    /* Verify the survivors by content, not just by size: every byte that should
     * have survived must still hold its pattern value. If the file came back
     * shorter than it should be, only the bytes that are left can be checked,
     * and the size check above has already recorded the failure. */
    GoodBytes = CountGoodBytes(Path,
                               (ExpectedEndOfFile <= ActualEndOfFile)
                                   ? ExpectedEndOfFile : ActualEndOfFile);
    sprintf(Name, "%s.content", CaseName);
    if (GoodBytes < 0)
        Skip(Name, "could not read the test file back");
    else
        Check(Name, ExpectedEndOfFile, (ULONGLONG)GoodBytes);

    DeleteFileW(Path);
}

int
main(int argc, char **argv)
{
    WCHAR Directory[MAX_PATH];
    WCHAR Path[MAX_PATH];
    WCHAR Root[MAX_PATH];
    DWORD SectorsPerCluster, BytesPerSector, FreeClusters, TotalClusters;
    ULONG ClusterSize;

    if (argc > 1)
    {
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, Directory, MAX_PATH);
    }
    else if (GetCurrentDirectoryW(MAX_PATH, Directory) == 0)
    {
        printf("Cannot determine the current directory\n");
        return 1;
    }

    if (Directory[0] == L'\0')
    {
        printf("Empty target directory\n");
        return 1;
    }

    if (!GetVolumePathNameW(Directory, Root, MAX_PATH))
    {
        printf("Cannot determine the volume of %S\n", Directory);
        return 1;
    }

    if (!GetDiskFreeSpaceW(Root, &SectorsPerCluster, &BytesPerSector,
                           &FreeClusters, &TotalClusters))
    {
        printf("Cannot determine the cluster size of %S\n", Root);
        return 1;
    }

    ClusterSize = SectorsPerCluster * BytesPerSector;
    if (ClusterSize == 0 || (ClusterSize & (ClusterSize - 1)) != 0)
    {
        printf("Implausible cluster size %lu on %S\n", ClusterSize, Root);
        return 1;
    }

    wcscpy(Path, Directory);
    if (Path[wcslen(Path) - 1] != L'\\')
        wcscat(Path, L"\\");
    wcscat(Path, L"ntfs_allocinfo.tmp");

    printf("Target %S, cluster size %lu\n", Path, ClusterSize);

    /* Case 1 is the discriminating one: the request rounds up to exactly the
     * space the file already occupies, so nothing may change. The old
     * fall-through truncated to 100 bytes here and destroyed the rest. */
    RunCase(Path, ClusterSize, "small_request_within_cluster", ClusterSize, 100);

    /* Case 2 exposes the cluster rounding: the file must shrink to one cluster,
     * not to the literal 100 bytes asked for. */
    RunCase(Path, ClusterSize, "small_request_across_clusters", ClusterSize * 4, 100);

    /* Case 3 is the negative control: a cluster-aligned shrink, which the old
     * fall-through already got right. It cannot discriminate on its own. */
    RunCase(Path, ClusterSize, "aligned_shrink", ClusterSize * 4, ClusterSize * 2);

    /* Case 4: a zero allocation really does truncate the file away. */
    RunCase(Path, ClusterSize, "zero_request", ClusterSize, 0);

    /* Case 5: an allocation above the file size must not extend the file. The
     * old fall-through grew the end of file to match. */
    RunCase(Path, ClusterSize, "grow_must_not_extend_eof", 0, ClusterSize);

    /* Case 6: same, with a non-empty file whose size is not cluster-aligned. */
    RunCase(Path, ClusterSize, "grow_keeps_unaligned_eof", 64, ClusterSize);

    printf("CHECKS EXECUTED=%lu PASSED=%lu FAILED=%lu\n",
           g_Executed, g_Passed, g_Failed);

    return (g_Failed == 0) ? 0 : 1;
}

/* EOF */
