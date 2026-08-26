/*
 *  ReactOS kernel
 *  Copyright (C) 2002 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/dirctl.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Hervé Poussineau (hpoussin@reactos.org)
 *                   Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Retrieve the standard file information
 */
static
NTSTATUS
NtfsGetStandardInformation(PNTFS_FCB Fcb,
                           PDEVICE_OBJECT DeviceObject,
                           PFILE_STANDARD_INFORMATION StandardInfo,
                           PULONG BufferLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    DPRINT("NtfsGetStandardInformation(%p, %p, %p, %p)\n", Fcb, DeviceObject, StandardInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_STANDARD_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    /* PRECONDITION */
    ASSERT(StandardInfo != NULL);
    ASSERT(Fcb != NULL);

    RtlZeroMemory(StandardInfo,
                  sizeof(FILE_STANDARD_INFORMATION));

    StandardInfo->AllocationSize = Fcb->RFCB.AllocationSize;
    StandardInfo->EndOfFile = Fcb->RFCB.FileSize;
    StandardInfo->NumberOfLinks = Fcb->LinkCount;
    StandardInfo->DeletePending = FALSE;
    StandardInfo->Directory = NtfsFCBIsDirectory(Fcb);

    *BufferLength -= sizeof(FILE_STANDARD_INFORMATION);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetPositionInformation(PFILE_OBJECT FileObject,
                           PFILE_POSITION_INFORMATION PositionInfo,
                           PULONG BufferLength)
{
    DPRINT1("NtfsGetPositionInformation(%p, %p, %p)\n", FileObject, PositionInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_POSITION_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    PositionInfo->CurrentByteOffset.QuadPart = FileObject->CurrentByteOffset.QuadPart;

    DPRINT("Getting position %I64x\n",
           PositionInfo->CurrentByteOffset.QuadPart);

    *BufferLength -= sizeof(FILE_POSITION_INFORMATION);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetBasicInformation(PFILE_OBJECT FileObject,
                        PNTFS_FCB Fcb,
                        PDEVICE_OBJECT DeviceObject,
                        PFILE_BASIC_INFORMATION BasicInfo,
                        PULONG BufferLength)
{
    PFILENAME_ATTRIBUTE FileName = &Fcb->Entry;

    DPRINT("NtfsGetBasicInformation(%p, %p, %p, %p, %p)\n", FileObject, Fcb, DeviceObject, BasicInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_BASIC_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(BasicInfo, sizeof(FILE_BASIC_INFORMATION));

    BasicInfo->CreationTime.QuadPart = FileName->CreationTime;
    BasicInfo->LastAccessTime.QuadPart = FileName->LastAccessTime;
    BasicInfo->LastWriteTime.QuadPart = FileName->LastWriteTime;
    BasicInfo->ChangeTime.QuadPart = FileName->ChangeTime;

    NtfsFileFlagsToAttributes(FileName->FileAttributes, &BasicInfo->FileAttributes);

    *BufferLength -= sizeof(FILE_BASIC_INFORMATION);

    return STATUS_SUCCESS;
}


/*
 * FUNCTION: Retrieve the file name information
 */
static
NTSTATUS
NtfsGetNameInformation(PFILE_OBJECT FileObject,
                       PNTFS_FCB Fcb,
                       PDEVICE_OBJECT DeviceObject,
                       PFILE_NAME_INFORMATION NameInfo,
                       PULONG BufferLength)
{
    ULONG BytesToCopy;

    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(DeviceObject);

    DPRINT("NtfsGetNameInformation(%p, %p, %p, %p, %p)\n", FileObject, Fcb, DeviceObject, NameInfo, BufferLength);

    ASSERT(NameInfo != NULL);
    ASSERT(Fcb != NULL);

    /* If buffer can't hold at least the file name length, bail out */
    if (*BufferLength < (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
        return STATUS_BUFFER_TOO_SMALL;

    /* Save file name length, and as much file len, as buffer length allows */
    NameInfo->FileNameLength = wcslen(Fcb->PathName) * sizeof(WCHAR);

    /* Calculate amount of bytes to copy not to overflow the buffer */
    BytesToCopy = min(NameInfo->FileNameLength,
                      *BufferLength - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]));

    /* Fill in the bytes */
    RtlCopyMemory(NameInfo->FileName, Fcb->PathName, BytesToCopy);

    /* Check if we could write more but are not able to */
    if (*BufferLength < NameInfo->FileNameLength + (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
    {
        /* Return number of bytes written */
        *BufferLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + BytesToCopy;
        return STATUS_BUFFER_OVERFLOW;
    }

    /* We filled up as many bytes, as needed */
    *BufferLength -= (FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + NameInfo->FileNameLength);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetInternalInformation(PNTFS_FCB Fcb,
                           PFILE_INTERNAL_INFORMATION InternalInfo,
                           PULONG BufferLength)
{
    DPRINT1("NtfsGetInternalInformation(%p, %p, %p)\n", Fcb, InternalInfo, BufferLength);

    ASSERT(InternalInfo);
    ASSERT(Fcb);

    if (*BufferLength < sizeof(FILE_INTERNAL_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    InternalInfo->IndexNumber.QuadPart = Fcb->MFTIndex;

    *BufferLength -= sizeof(FILE_INTERNAL_INFORMATION);

    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsGetNetworkOpenInformation(PNTFS_FCB Fcb,
                              PDEVICE_EXTENSION DeviceExt,
                              PFILE_NETWORK_OPEN_INFORMATION NetworkInfo,
                              PULONG BufferLength)
{
    PFILENAME_ATTRIBUTE FileName = &Fcb->Entry;

    DPRINT("NtfsGetNetworkOpenInformation(%p, %p, %p, %p)\n", Fcb, DeviceExt, NetworkInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_NETWORK_OPEN_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    NetworkInfo->CreationTime.QuadPart = FileName->CreationTime;
    NetworkInfo->LastAccessTime.QuadPart = FileName->LastAccessTime;
    NetworkInfo->LastWriteTime.QuadPart = FileName->LastWriteTime;
    NetworkInfo->ChangeTime.QuadPart = FileName->ChangeTime;

    NetworkInfo->EndOfFile = Fcb->RFCB.FileSize;
    NetworkInfo->AllocationSize = Fcb->RFCB.AllocationSize;

    NtfsFileFlagsToAttributes(FileName->FileAttributes, &NetworkInfo->FileAttributes);

    *BufferLength -= sizeof(FILE_NETWORK_OPEN_INFORMATION);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsGetStreamInformation(PNTFS_FCB Fcb,
                         PDEVICE_EXTENSION DeviceExt,
                         PFILE_STREAM_INFORMATION StreamInfo,
                         PULONG BufferLength)
{
    ULONG CurrentSize;
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;
    NTSTATUS Status, BrowseStatus;
    PFILE_RECORD_HEADER FileRecord;
    PFILE_STREAM_INFORMATION CurrentInfo = StreamInfo, Previous = NULL;

    if (*BufferLength < sizeof(FILE_STREAM_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        DPRINT1("Not enough memory!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Can't find record!\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    BrowseStatus = FindFirstAttribute(&Context, DeviceExt, FileRecord, FALSE, &Attribute);
    while (NT_SUCCESS(BrowseStatus))
    {
        if (Attribute->Type == AttributeData)
        {
            CurrentSize = FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName) + Attribute->NameLength * sizeof(WCHAR) + wcslen(L"::$DATA") * sizeof(WCHAR);

            if (CurrentSize > *BufferLength)
            {
                Status = STATUS_BUFFER_OVERFLOW;
                break;
            }

            CurrentInfo->NextEntryOffset = 0;
            CurrentInfo->StreamNameLength = (Attribute->NameLength + wcslen(L"::$DATA")) * sizeof(WCHAR);
            CurrentInfo->StreamSize.QuadPart = AttributeDataLength(Attribute);
            CurrentInfo->StreamAllocationSize.QuadPart = AttributeAllocatedLength(Attribute);
            CurrentInfo->StreamName[0] = L':';
            RtlMoveMemory(&CurrentInfo->StreamName[1], (PWCHAR)((ULONG_PTR)Attribute + Attribute->NameOffset), CurrentInfo->StreamNameLength);
            RtlMoveMemory(&CurrentInfo->StreamName[Attribute->NameLength + 1], L":$DATA", sizeof(L":$DATA") - sizeof(UNICODE_NULL));

            if (Previous != NULL)
            {
                Previous->NextEntryOffset = (ULONG_PTR)CurrentInfo - (ULONG_PTR)Previous;
            }
            Previous = CurrentInfo;
            CurrentInfo = (PFILE_STREAM_INFORMATION)((ULONG_PTR)CurrentInfo + CurrentSize);
            *BufferLength -= CurrentSize;
        }

        BrowseStatus = FindNextAttribute(&Context, &Attribute);
    }

    FindCloseAttribute(&Context);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}

// Convert enum value to friendly name
const PCSTR
GetInfoClassName(FILE_INFORMATION_CLASS infoClass)
{
    const PCSTR fileInfoClassNames[] = { "???????",
        "FileDirectoryInformation",
        "FileFullDirectoryInformation",
        "FileBothDirectoryInformation",
        "FileBasicInformation",
        "FileStandardInformation",
        "FileInternalInformation",
        "FileEaInformation",
        "FileAccessInformation",
        "FileNameInformation",
        "FileRenameInformation",
        "FileLinkInformation",
        "FileNamesInformation",
        "FileDispositionInformation",
        "FilePositionInformation",
        "FileFullEaInformation",
        "FileModeInformation",
        "FileAlignmentInformation",
        "FileAllInformation",
        "FileAllocationInformation",
        "FileEndOfFileInformation",
        "FileAlternateNameInformation",
        "FileStreamInformation",
        "FilePipeInformation",
        "FilePipeLocalInformation",
        "FilePipeRemoteInformation",
        "FileMailslotQueryInformation",
        "FileMailslotSetInformation",
        "FileCompressionInformation",
        "FileObjectIdInformation",
        "FileCompletionInformation",
        "FileMoveClusterInformation",
        "FileQuotaInformation",
        "FileReparsePointInformation",
        "FileNetworkOpenInformation",
        "FileAttributeTagInformation",
        "FileTrackingInformation",
        "FileIdBothDirectoryInformation",
        "FileIdFullDirectoryInformation",
        "FileValidDataLengthInformation",
        "FileShortNameInformation",
        "FileIoCompletionNotificationInformation",
        "FileIoStatusBlockRangeInformation",
        "FileIoPriorityHintInformation",
        "FileSfioReserveInformation",
        "FileSfioVolumeInformation",
        "FileHardLinkInformation",
        "FileProcessIdsUsingFileInformation",
        "FileNormalizedNameInformation",
        "FileNetworkPhysicalNameInformation",
        "FileIdGlobalTxDirectoryInformation",
        "FileIsRemoteDeviceInformation",
        "FileAttributeCacheInformation",
        "FileNumaNodeInformation",
        "FileStandardLinkInformation",
        "FileRemoteProtocolInformation",
        "FileReplaceCompletionInformation",
        "FileMaximumInformation",
        "FileDirectoryInformation",
        "FileFullDirectoryInformation",
        "FileBothDirectoryInformation",
        "FileBasicInformation",
        "FileStandardInformation",
        "FileInternalInformation",
        "FileEaInformation",
        "FileAccessInformation",
        "FileNameInformation",
        "FileRenameInformation",
        "FileLinkInformation",
        "FileNamesInformation",
        "FileDispositionInformation",
        "FilePositionInformation",
        "FileFullEaInformation",
        "FileModeInformation",
        "FileAlignmentInformation",
        "FileAllInformation",
        "FileAllocationInformation",
        "FileEndOfFileInformation",
        "FileAlternateNameInformation",
        "FileStreamInformation",
        "FilePipeInformation",
        "FilePipeLocalInformation",
        "FilePipeRemoteInformation",
        "FileMailslotQueryInformation",
        "FileMailslotSetInformation",
        "FileCompressionInformation",
        "FileObjectIdInformation",
        "FileCompletionInformation",
        "FileMoveClusterInformation",
        "FileQuotaInformation",
        "FileReparsePointInformation",
        "FileNetworkOpenInformation",
        "FileAttributeTagInformation",
        "FileTrackingInformation",
        "FileIdBothDirectoryInformation",
        "FileIdFullDirectoryInformation",
        "FileValidDataLengthInformation",
        "FileShortNameInformation",
        "FileIoCompletionNotificationInformation",
        "FileIoStatusBlockRangeInformation",
        "FileIoPriorityHintInformation",
        "FileSfioReserveInformation",
        "FileSfioVolumeInformation",
        "FileHardLinkInformation",
        "FileProcessIdsUsingFileInformation",
        "FileNormalizedNameInformation",
        "FileNetworkPhysicalNameInformation",
        "FileIdGlobalTxDirectoryInformation",
        "FileIsRemoteDeviceInformation",
        "FileAttributeCacheInformation",
        "FileNumaNodeInformation",
        "FileStandardLinkInformation",
        "FileRemoteProtocolInformation",
        "FileReplaceCompletionInformation",
        "FileMaximumInformation" };
    return fileInfoClassNames[infoClass];
}

/*
 * FUNCTION: Retrieve the specified file information
 */
NTSTATUS
NtfsQueryInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    FILE_INFORMATION_CLASS FileInformationClass;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PVOID SystemBuffer;
    ULONG BufferLength;
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT1("NtfsQueryInformation(%p)\n", IrpContext);

    Irp = IrpContext->Irp;
    Stack = IrpContext->Stack;
    DeviceObject = IrpContext->DeviceObject;
    FileInformationClass = Stack->Parameters.QueryFile.FileInformationClass;
    FileObject = IrpContext->FileObject;
    Fcb = FileObject->FsContext;

    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    BufferLength = Stack->Parameters.QueryFile.Length;

    if (!ExAcquireResourceSharedLite(&Fcb->MainResource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    switch (FileInformationClass)
    {
        case FileStandardInformation:
            Status = NtfsGetStandardInformation(Fcb,
                                                DeviceObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FilePositionInformation:
            Status = NtfsGetPositionInformation(FileObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileBasicInformation:
            Status = NtfsGetBasicInformation(FileObject,
                                             Fcb,
                                             DeviceObject,
                                             SystemBuffer,
                                             &BufferLength);
            break;

        case FileNameInformation:
            Status = NtfsGetNameInformation(FileObject,
                                            Fcb,
                                            DeviceObject,
                                            SystemBuffer,
                                            &BufferLength);
            break;

        case FileInternalInformation:
            Status = NtfsGetInternalInformation(Fcb,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileNetworkOpenInformation:
            Status = NtfsGetNetworkOpenInformation(Fcb,
                                                   DeviceObject->DeviceExtension,
                                                   SystemBuffer,
                                                   &BufferLength);
            break;

        case FileStreamInformation:
            Status = NtfsGetStreamInformation(Fcb,
                                              DeviceObject->DeviceExtension,
                                              SystemBuffer,
                                              &BufferLength);
            break;

        case FileAlternateNameInformation:
        case FileAllInformation:
            DPRINT1("Unimplemented information class: %s\n", GetInfoClassName(FileInformationClass));
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        default:
            DPRINT1("Unimplemented information class: %s\n", GetInfoClassName(FileInformationClass));
            Status = STATUS_INVALID_PARAMETER;
    }

    ExReleaseResourceLite(&Fcb->MainResource);

    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information =
            Stack->Parameters.QueryFile.Length - BufferLength;
    else
        Irp->IoStatus.Information = 0;

    return Status;
}

/**
* @name NtfsSetEndOfFile
* @implemented
*
* Sets the end of file (file size) for a given file.
*
* @param Fcb
* Pointer to an NTFS_FCB which describes the target file. Fcb->MainResource should have been
* acquired with ExAcquireResourceSharedLite().
*
* @param FileObject
* Pointer to a FILE_OBJECT describing the target file.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION
*
* @param IrpFlags
* ULONG describing the flags of the original IRP request (Irp->Flags).
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application opened the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @param TruncateOnly
* Boolean indicating that the file must never be enlarged. When TRUE, a NewFileSize which is
* greater than or equal to the current size of the data attribute leaves the file untouched and
* STATUS_SUCCESS is returned. This is what FileAllocationInformation needs: setting an allocation
* size can shrink a file, but it must never grow the end-of-file position.
*
* @param NewFileSize
* Pointer to a LARGE_INTEGER which indicates the new end of file (file size).
*
* @return
* STATUS_SUCCESS if successful,
* STATUS_USER_MAPPED_FILE if trying to truncate a file but MmCanFileBeTruncated() returned false,
* STATUS_OBJECT_NAME_NOT_FOUND if there was no $DATA attribute associated with the target file,
* STATUS_INVALID_PARAMETER if there was no $FILENAME attribute associated with the target file,
* STATUS_INSUFFICIENT_RESOURCES if an allocation failed,
* STATUS_ACCESS_DENIED if target file is a volume or if paging is involved.
*
* @remarks As this function sets the size of a file at the file-level
* (and not at the attribute level) it's not recommended to use this
* function alongside functions that operate on the data attribute directly.
*
*/
NTSTATUS
NtfsSetEndOfFile(PNTFS_FCB Fcb,
                 PFILE_OBJECT FileObject,
                 PDEVICE_EXTENSION DeviceExt,
                 ULONG IrpFlags,
                 BOOLEAN CaseSensitive,
                 BOOLEAN TruncateOnly,
                 PLARGE_INTEGER NewFileSize)
{
    LARGE_INTEGER CurrentFileSize;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONG AttributeOffset;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONGLONG AllocationSize;
    PFILENAME_ATTRIBUTE FileNameAttribute;
    ULONGLONG ParentMFTId;
    UNICODE_STRING FileName;


    // Allocate non-paged memory for the file record
    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        DPRINT1("Couldn't allocate memory for file record!");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // read the file record
    DPRINT("Reading file record...\n");
    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        // We couldn't get the file's record. Free the memory and return the error
        DPRINT1("Can't find record for %wS!\n", Fcb->ObjectName);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    DPRINT("Found record for %wS\n", Fcb->ObjectName);

    CurrentFileSize.QuadPart = NtfsGetFileSize(DeviceExt, FileRecord, L"", 0, NULL);

    // Are we trying to decrease the file size?
    if (NewFileSize->QuadPart < CurrentFileSize.QuadPart)
    {
        // Is the file mapped?
        if (!MmCanFileBeTruncated(FileObject->SectionObjectPointer,
                                  NewFileSize))
        {
            DPRINT1("Couldn't decrease file size!\n");
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            return STATUS_USER_MAPPED_FILE;
        }
    }

    // Find the attribute with the data stream for our file
    DPRINT("Finding Data Attribute...\n");
    Status = FindAttribute(DeviceExt,
                           FileRecord,
                           AttributeData,
                           Fcb->Stream,
                           wcslen(Fcb->Stream),
                           &DataContext,
                           &AttributeOffset);

    // Did we fail to find the attribute?
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No '%S' data stream associated with file!\n", Fcb->Stream);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // Get the size of the data attribute
    CurrentFileSize.QuadPart = AttributeDataLength(DataContext->pRecord);

    // Our caller may forbid us from ever growing the file (FileAllocationInformation).
    // This is checked against the attribute length rather than against the cached
    // Fcb->RFCB.FileSize, so that a stale cached size can never turn into a file being
    // extended behind the caller's back.
    if (TruncateOnly && NewFileSize->QuadPart >= CurrentFileSize.QuadPart)
    {
        DPRINT("Leaving file size at %I64u; requested %I64u is not a truncation\n",
               CurrentFileSize.QuadPart, NewFileSize->QuadPart);
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_SUCCESS;
    }

    // Are we enlarging the attribute?
    if (NewFileSize->QuadPart > CurrentFileSize.QuadPart)
    {
        // is increasing the stream size not allowed?
        if ((Fcb->Flags & FCB_IS_VOLUME) ||
            (IrpFlags & IRP_PAGING_IO))
        {
            // TODO - just fail for now
            ReleaseAttributeContext(DataContext);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            return STATUS_ACCESS_DENIED;
        }
    }

    // set the attribute data length
    Status = SetAttributeDataLength(FileObject, Fcb, DataContext, AttributeOffset, FileRecord, NewFileSize);
    if (!NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // now we need to update this file's size in every directory index entry that references it
    // TODO: expand to work with every filename / hardlink stored in the file record.
    FileNameAttribute = GetBestFileNameFromRecord(Fcb->Vcb, FileRecord);
    if (FileNameAttribute == NULL)
    {
        DPRINT1("Unable to find FileName attribute associated with file!\n");
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_INVALID_PARAMETER;
    }

    ParentMFTId = FileNameAttribute->DirectoryFileReferenceNumber & NTFS_MFT_MASK;

    FileName.Buffer = FileNameAttribute->Name;
    FileName.Length = FileNameAttribute->NameLength * sizeof(WCHAR);
    FileName.MaximumLength = FileName.Length;

    AllocationSize = AttributeAllocatedLength(DataContext->pRecord);

    Status = UpdateFileNameRecord(Fcb->Vcb,
                                  ParentMFTId,
                                  &FileName,
                                  FALSE,
                                  NewFileSize->QuadPart,
                                  AllocationSize,
                                  CaseSensitive);

    ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsSetInformation
* @implemented
*
* Sets the specified file information.
*
* @param IrpContext
* Points to an NTFS_IRP_CONTEXT which describes the set operation
*
* @return
* STATUS_SUCCESS if successful,
* STATUS_NOT_IMPLEMENTED if trying to set an unimplemented information class,
* STATUS_USER_MAPPED_FILE if trying to truncate a file but MmCanFileBeTruncated() returned false,
* STATUS_OBJECT_NAME_NOT_FOUND if there was no $DATA attribute associated with the target file,
* STATUS_INVALID_PARAMETER if there was no $FILENAME attribute associated with the target file,
* STATUS_INSUFFICIENT_RESOURCES if an allocation failed,
* STATUS_ACCESS_DENIED if target file is a volume or if paging is involved.
*
* @remarks Called by NtfsDispatch() in response to an IRP_MJ_SET_INFORMATION request.
* Only the FileEndOfFileInformation InformationClass is fully implemented.
* FileAllocationInformation gets the end-of-file half of its contract right -- it rounds the
* request up to a cluster boundary and truncates only when that lands below the current file
* size -- but it cannot grow the on-disk allocation beyond the file size, because this driver
* has no way to hold allocated-but-unused clusters.
* All other information classes are TODO.
*
*/
NTSTATUS
NtfsSetInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    FILE_INFORMATION_CLASS FileInformationClass;
    PIO_STACK_LOCATION Stack;
    PDEVICE_EXTENSION DeviceExt;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PVOID SystemBuffer;
    ULONG BufferLength;
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    PFILE_ALLOCATION_INFORMATION AllocationInfo;
    LARGE_INTEGER AllocationSize;
    ULONG BytesPerCluster;
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

    DPRINT("NtfsSetInformation(%p)\n", IrpContext);

    Irp = IrpContext->Irp;
    Stack = IrpContext->Stack;
    DeviceObject = IrpContext->DeviceObject;
    DeviceExt = DeviceObject->DeviceExtension;
    FileInformationClass = Stack->Parameters.QueryFile.FileInformationClass;
    FileObject = IrpContext->FileObject;
    Fcb = FileObject->FsContext;

    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    BufferLength = Stack->Parameters.QueryFile.Length;

    if (!ExAcquireResourceSharedLite(&Fcb->MainResource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    switch (FileInformationClass)
    {
        PFILE_END_OF_FILE_INFORMATION EndOfFileInfo;

        /* Real Windows treats FileAllocationInformation and FileEndOfFileInformation
           very differently. Measured on Windows 11 Pro 22621, NTFS with 4096-byte
           clusters, by querying FILE_STANDARD_INFORMATION before and after the set
           (identical results under WOW64 and native x86_64):

             initial EndOfFile | requested allocation | result
             ------------------+----------------------+-------------------------
                             0 |                 4096 | EndOfFile stays 0,
                               |                      | AllocationSize 0 -> 4096
                            64 |                 4096 | EndOfFile stays 64,
                               |                      | AllocationSize 64 -> 4096
                          4096 |                  100 | nothing changes at all
                          4096 |                    0 | EndOfFile 4096 -> 0
                         16384 |                  100 | EndOfFile 16384 -> 4096
                         16384 |                 8192 | EndOfFile 16384 -> 8192
                         16384 |                16384 | nothing changes

           The rule: round the requested allocation up to the cluster size; if the
           rounded value is below EndOfFile, truncate the file to it; otherwise leave
           the file size alone and only grow the allocation. This matches the
           documented contract in ntifs.h's Remarks for FILE_ALLOCATION_INFORMATION:
           "The end-of-file position must always be less than or equal to the
           allocation size. If the allocation size is set to a value that is less than
           the end-of-file position, the end-of-file position is automatically adjusted
           to match the allocation size."

           This case used to fall through to FileEndOfFileInformation, which diverged
           in both directions. With EndOfFile 0 and a request of 4096 we set EndOfFile
           to 4096 where Windows leaves it at 0. Worse, with EndOfFile 4096 and a
           request of 100 we truncated to 100 and destroyed 3996 bytes that Windows
           preserves, because 100 rounds up to a single cluster which the file already
           occupies.

           What is implemented below is the EndOfFile half of the rule, which is the
           half that decides whether user data survives: round up, and truncate only on
           a genuine shrink. Every row of the table above now matches on EndOfFile.

           What is NOT implemented is the AllocationSize half: the first two rows grow
           AllocationSize past EndOfFile on Windows, and we leave both alone. Doing that
           would mean holding clusters that are allocated but not covered by the data
           attribute's length, which this driver has no representation for -- it derives
           Fcb->RFCB.AllocationSize straight from the attribute record (see mft.c). And
           reserving space is only a hint, so declining it is a completeness gap, while
           truncating a file the caller merely asked to reserve space for is data loss.
           fastfat, which does track the two separately, has the full shape: see
           FatSetAllocationInfo() in drivers/filesystems/fastfat/fileinfo.c.

           Note for anyone extending the test: the EndOfFile 4096 / request 100 row is
           the discriminating case. On its own it looks like "small requests are
           ignored", which is a clean, plausible and wrong rule; only the 16384 rows
           expose the cluster rounding, and a cluster-aligned request behaves the same
           either way. See modules/rostests/apitests/ntfs/. */
        case FileAllocationInformation:
            AllocationInfo = (PFILE_ALLOCATION_INFORMATION)SystemBuffer;
            BytesPerCluster = DeviceExt->NtfsInfo.BytesPerCluster;

            if (BufferLength < sizeof(FILE_ALLOCATION_INFORMATION) ||
                AllocationInfo->AllocationSize.QuadPart < 0 ||
                BytesPerCluster == 0)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            /* NTFS cluster sizes are always powers of two, so this is a mask-and-add
               and needs no 64-bit division helper */
            AllocationSize.QuadPart = ALIGN_UP_BY((ULONGLONG)AllocationInfo->AllocationSize.QuadPart,
                                                  (ULONGLONG)BytesPerCluster);

            DPRINT("Setting allocation size: requested %I64u, rounded %I64u, file size %I64u\n",
                   AllocationInfo->AllocationSize.QuadPart,
                   AllocationSize.QuadPart,
                   Fcb->RFCB.FileSize.QuadPart);

            if (AllocationSize.QuadPart >= Fcb->RFCB.FileSize.QuadPart)
            {
                /* Not a shrink. Windows would grow the allocation here and leave the
                   file size alone; we cannot hold an allocation of our own, so we leave
                   both alone. What we must not do is extend the file. */
                Status = STATUS_SUCCESS;
                break;
            }

            /* A genuine shrink: truncate to the cluster-rounded allocation. TruncateOnly
               is passed as well, so that a stale cached file size cannot turn this into
               a grow once the real attribute length is known. */
            Status = NtfsSetEndOfFile(Fcb,
                                      FileObject,
                                      DeviceExt,
                                      Irp->Flags,
                                      BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                      TRUE,
                                      &AllocationSize);
            break;

        case FileEndOfFileInformation:
            EndOfFileInfo = (PFILE_END_OF_FILE_INFORMATION)SystemBuffer;
            Status = NtfsSetEndOfFile(Fcb,
                                      FileObject,
                                      DeviceExt,
                                      Irp->Flags,
                                      BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                      FALSE,
                                      &EndOfFileInfo->EndOfFile);
            break;

        // TODO: all other information classes

        default:
            DPRINT1("FIXME: Unimplemented information class: %s\n", GetInfoClassName(FileInformationClass));
            Status = STATUS_NOT_IMPLEMENTED;
    }

    ExReleaseResourceLite(&Fcb->MainResource);

    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information =
        Stack->Parameters.QueryFile.Length - BufferLength;
    else
        Irp->IoStatus.Information = 0;

    return Status;
}
/* EOF */
