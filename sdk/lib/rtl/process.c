/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS system libraries
 * FILE:              lib/rtl/process.c
 * PURPOSE:           Process functions
 * PROGRAMMER:        Alex Ionescu (alex@relsoft.net)
 *                    Ariadne (ariadne@xs4all.nl)
 *                    Eric Kohl
 */

/* INCLUDES ****************************************************************/

#include <rtl.h>

#define NDEBUG
#include <debug.h>

/* INTERNAL FUNCTIONS *******************************************************/

NTSTATUS
NTAPI
RtlpMapFile(PUNICODE_STRING ImageFileName,
            ULONG Attributes,
            PHANDLE Section)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    HANDLE hFile = NULL;
    IO_STATUS_BLOCK IoStatusBlock;

    /* Open the Image File */
    InitializeObjectAttributes(&ObjectAttributes,
                               ImageFileName,
                               Attributes & (OBJ_CASE_INSENSITIVE | OBJ_INHERIT),
                               NULL,
                               NULL);
    Status = ZwOpenFile(&hFile,
                        SYNCHRONIZE | FILE_EXECUTE | FILE_READ_DATA,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_DELETE | FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to read image file from disk, Status = 0x%08X\n", Status);
        return Status;
    }

    /* Now create a section for this image */
    Status = ZwCreateSection(Section,
                             SECTION_ALL_ACCESS,
                             NULL,
                             NULL,
                             PAGE_EXECUTE,
                             SEC_IMAGE,
                             hFile);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create section for image file, Status = 0x%08X\n", Status);
    }

    ZwClose(hFile);
    return Status;
}

/* FUNCTIONS ****************************************************************/

NTSTATUS
NTAPI
RtlpInitEnvironment(HANDLE ProcessHandle,
                    PPEB Peb,
                    PRTL_USER_PROCESS_PARAMETERS ProcessParameters)
{
    NTSTATUS Status;
    PVOID BaseAddress = NULL;
    SIZE_T EnviroSize;
    SIZE_T Size;
    PWCHAR Environment = NULL;
    DPRINT("RtlpInitEnvironment(ProcessHandle: %p, Peb: %p Params: %p)\n",
            ProcessHandle, Peb, ProcessParameters);

    /* Give the caller 1MB if he requested it */
    if (ProcessParameters->Flags & RTL_USER_PROCESS_PARAMETERS_RESERVE_1MB)
    {
        /* Give 1MB starting at 0x4 */
        BaseAddress = (PVOID)4;
        EnviroSize = (1024 * 1024) - 256;
        Status = ZwAllocateVirtualMemory(ProcessHandle,
                                         &BaseAddress,
                                         0,
                                         &EnviroSize,
                                         MEM_RESERVE,
                                         PAGE_READWRITE);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to reserve 1MB of space\n");
            return Status;
        }
    }

    /* Find the end of the Enviroment Block */
    if ((Environment = (PWCHAR)ProcessParameters->Environment))
    {
        while (*Environment++) while (*Environment++);

        /* Calculate the size of the block */
        EnviroSize = (ULONG)((ULONG_PTR)Environment -
                             (ULONG_PTR)ProcessParameters->Environment);

        /* Allocate and Initialize new Environment Block */
        Size = EnviroSize;
        Status = ZwAllocateVirtualMemory(ProcessHandle,
                                         &BaseAddress,
                                         0,
                                         &Size,
                                         MEM_RESERVE | MEM_COMMIT,
                                         PAGE_READWRITE);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to allocate Environment Block\n");
            return Status;
        }

        /* Write the Environment Block */
        ZwWriteVirtualMemory(ProcessHandle,
                             BaseAddress,
                             ProcessParameters->Environment,
                             EnviroSize,
                             NULL);

        /* Save pointer */
        ProcessParameters->Environment = BaseAddress;
    }

    /* Now allocate space for the Parameter Block */
    BaseAddress = NULL;
    Size = ProcessParameters->MaximumLength;
    Status = ZwAllocateVirtualMemory(ProcessHandle,
                                     &BaseAddress,
                                     0,
                                     &Size,
                                     MEM_COMMIT,
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to allocate Parameter Block\n");
        return Status;
    }

    /* Write the Parameter Block */
    Status = ZwWriteVirtualMemory(ProcessHandle,
                                  BaseAddress,
                                  ProcessParameters,
                                  ProcessParameters->Length,
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to write the Parameter Block\n");
        return Status;
    }

    /* Write pointer to Parameter Block */
    Status = ZwWriteVirtualMemory(ProcessHandle,
                                  &Peb->ProcessParameters,
                                  &BaseAddress,
                                  sizeof(BaseAddress),
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to write pointer to Parameter Block\n");
        return Status;
    }

    /* Return */
    return STATUS_SUCCESS;
}

/*
 * @implemented
 *
 * Creates a process and its initial thread.
 *
 * NOTES:
 *  - The first thread is created suspended, so it needs a manual resume!!!
 *  - If ParentProcess is NULL, current process is used
 *  - ProcessParameters must be normalized
 *  - Attributes are object attribute flags used when opening the ImageFileName.
 *    Valid flags are OBJ_INHERIT and OBJ_CASE_INSENSITIVE.
 *
 * -Gunnar
 */
NTSTATUS
NTAPI
RtlCreateUserProcess(IN PUNICODE_STRING ImageFileName,
                     IN ULONG Attributes,
                     IN OUT PRTL_USER_PROCESS_PARAMETERS ProcessParameters,
                     IN PSECURITY_DESCRIPTOR ProcessSecurityDescriptor OPTIONAL,
                     IN PSECURITY_DESCRIPTOR ThreadSecurityDescriptor OPTIONAL,
                     IN HANDLE ParentProcess OPTIONAL,
                     IN BOOLEAN InheritHandles,
                     IN HANDLE DebugPort OPTIONAL,
                     IN HANDLE ExceptionPort OPTIONAL,
                     OUT PRTL_USER_PROCESS_INFORMATION ProcessInfo)
{
    NTSTATUS Status;
    HANDLE hSection;
    PROCESS_BASIC_INFORMATION ProcessBasicInfo;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING DebugString = RTL_CONSTANT_STRING(L"\\WindowsSS");
    DPRINT("RtlCreateUserProcess: %wZ\n", ImageFileName);

    /* Map and Load the File */
    Status = RtlpMapFile(ImageFileName,
                         Attributes,
                         &hSection);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Could not map process image\n");
        return Status;
    }

    /* Clean out the current directory handle if we won't use it */
    if (!InheritHandles) ProcessParameters->CurrentDirectory.Handle = NULL;

    /* Use us as parent if none other specified */
    if (!ParentProcess) ParentProcess = NtCurrentProcess();

    /* Initialize the Object Attributes */
    InitializeObjectAttributes(&ObjectAttributes,
                               NULL,
                               0,
                               NULL,
                               ProcessSecurityDescriptor);

    /*
     * If FLG_ENABLE_CSRDEBUG is used, then CSRSS is created under the
     * watch of WindowsSS
     */
    if ((RtlGetNtGlobalFlags() & FLG_ENABLE_CSRDEBUG) &&
        (wcsstr(ImageFileName->Buffer, L"csrss")))
    {
        ObjectAttributes.ObjectName = &DebugString;
    }

    /* Create Kernel Process Object */
    Status = ZwCreateProcess(&ProcessInfo->ProcessHandle,
                             PROCESS_ALL_ACCESS,
                             &ObjectAttributes,
                             ParentProcess,
                             InheritHandles,
                             hSection,
                             DebugPort,
                             ExceptionPort);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Could not create Kernel Process Object\n");
        ZwClose(hSection);
        return Status;
    }

    /* Get some information on the image */
    Status = ZwQuerySection(hSection,
                            SectionImageInformation,
                            &ProcessInfo->ImageInformation,
                            sizeof(SECTION_IMAGE_INFORMATION),
                            NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Could not query Section Info\n");
        ZwClose(ProcessInfo->ProcessHandle);
        ZwClose(hSection);
        return Status;
    }

    /* Get some information about the process */
    Status = ZwQueryInformationProcess(ProcessInfo->ProcessHandle,
                                       ProcessBasicInformation,
                                       &ProcessBasicInfo,
                                       sizeof(ProcessBasicInfo),
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Could not query Process Info\n");
        ZwClose(ProcessInfo->ProcessHandle);
        ZwClose(hSection);
        return Status;
    }

    /* Duplicate the standard handles */
    Status = STATUS_SUCCESS;
    _SEH2_TRY
    {
        if (ProcessParameters->StandardInput)
        {
            Status = ZwDuplicateObject(ParentProcess,
                                       ProcessParameters->StandardInput,
                                       ProcessInfo->ProcessHandle,
                                       &ProcessParameters->StandardInput,
                                       0,
                                       0,
                                       DUPLICATE_SAME_ACCESS |
                                       DUPLICATE_SAME_ATTRIBUTES);
            if (!NT_SUCCESS(Status))
            {
                _SEH2_LEAVE;
            }
        }

        if (ProcessParameters->StandardOutput)
        {
            Status = ZwDuplicateObject(ParentProcess,
                                       ProcessParameters->StandardOutput,
                                       ProcessInfo->ProcessHandle,
                                       &ProcessParameters->StandardOutput,
                                       0,
                                       0,
                                       DUPLICATE_SAME_ACCESS |
                                       DUPLICATE_SAME_ATTRIBUTES);
            if (!NT_SUCCESS(Status))
            {
                _SEH2_LEAVE;
            }
        }

        if (ProcessParameters->StandardError)
        {
            Status = ZwDuplicateObject(ParentProcess,
                                       ProcessParameters->StandardError,
                                       ProcessInfo->ProcessHandle,
                                       &ProcessParameters->StandardError,
                                       0,
                                       0,
                                       DUPLICATE_SAME_ACCESS |
                                       DUPLICATE_SAME_ATTRIBUTES);
            if (!NT_SUCCESS(Status))
            {
                _SEH2_LEAVE;
            }
        }
    }
    _SEH2_FINALLY
    {
        if (!NT_SUCCESS(Status))
        {
            ZwClose(ProcessInfo->ProcessHandle);
            ZwClose(hSection);
        }
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
        return Status;

    /* Create Process Environment */
    Status = RtlpInitEnvironment(ProcessInfo->ProcessHandle,
                                 ProcessBasicInfo.PebBaseAddress,
                                 ProcessParameters);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Could not Create Process Environment\n");
        ZwClose(ProcessInfo->ProcessHandle);
        ZwClose(hSection);
        return Status;
    }

    /* Create the first Thread */
    Status = RtlCreateUserThread(ProcessInfo->ProcessHandle,
                                 ThreadSecurityDescriptor,
                                 TRUE,
                                 ProcessInfo->ImageInformation.ZeroBits,
                                 ProcessInfo->ImageInformation.MaximumStackSize,
                                 ProcessInfo->ImageInformation.CommittedStackSize,
                                 ProcessInfo->ImageInformation.TransferAddress,
                                 ProcessBasicInfo.PebBaseAddress,
                                 &ProcessInfo->ThreadHandle,
                                 &ProcessInfo->ClientId);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Could not Create Thread\n");
        ZwClose(ProcessInfo->ProcessHandle);
        ZwClose(hSection); /* Don't try to optimize this on top! */
        return Status;
    }

    /* Close the Section Handle and return */
    ZwClose(hSection);
    return STATUS_SUCCESS;
}

/*
 * @implemented
 *
 * Duplicates ("clones", cf. POSIX fork()) the calling process into a new
 * process, whose single initial thread starts suspended and is itself a
 * clone of the calling thread. See the header comment on the prototype in
 * <ndk/rtlfuncs.h> for the parent/child return-value contract.
 *
 * ARCHITECTURE:
 *
 * Unlike RtlCreateUserProcess() above, this does not map a new image: it
 * asks ZwCreateProcessClone() to clone the CALLING process's own address
 * space AND, in the same call, the calling thread's own trap context into
 * the child's initial thread. ZwCreateProcessClone() is a genuinely new,
 * ReactOS-specific syscall (not present in real NT) that wraps
 * PsCreateCloneProcess() (ntoskrnl/ps/process.c); see the comment on that
 * function's prototype in ntoskrnl/include/internal/ps.h for exactly how
 * its service number is assigned (via the shared ntoskrnl/include/sysfuncs.h
 * table, so ntoskrnl's MainSSDT and ntdll's generated stub always agree by
 * construction). Under the hood it is still the same legacy fork()-emulation
 * trick real pre-Vista NT exposed via NtCreateProcess() (used by e.g.
 * Interix and Cygwin's fork()): SectionHandle = NULL together with a Parent
 * (ourselves) is what PspCreateProcess recognizes as "this is a clone!" and
 * answers by duplicating Parent's inheritable VADs (heap, stacks, and --
 * since the PEB's own VAD is PrivateMemory -- the PEB too) into the child
 * via MmCloneAddressSpace() (ntoskrnl/mm/ARM3/procsup.c).
 *
 * Why a whole new syscall instead of reusing plain NtCreateProcess()/
 * NtCreateProcessEx(): those two are real NT ABI (their parameter lists are
 * fixed by every existing caller, in this codebase and in real Windows
 * binaries), and have no room for the extra ThreadHandle/ClientId output
 * that PspCreateProcess's clone branch can produce in the very same call
 * (a new initial thread whose context is a copy of the caller's own,
 * patched to return STATUS_PROCESS_CLONED). PsCreateCloneProcess() is that
 * third parameter list, and NtCreateProcessClone()/ZwCreateProcessClone()
 * (ntoskrnl/ps/process.c, right after NtCreateProcessEx) are its syscall
 * wrapper.
 *
 * (An easier-looking alternative -- capture our own context with
 * NtGetContextThread(NtCurrentThread(), ...) right here, patch it by hand,
 * and feed it to a second, ordinary ZwCreateThread() call -- was considered
 * and deliberately NOT used: the whole point of doing this in the kernel,
 * inside the SAME syscall that already has the calling thread's trap frame
 * live, is that the resume point is exactly "return from the syscall that
 * got us here", which needs no assumption about what ntdll's C compiler
 * does with a register afterwards. A second, separate NtGetContextThread()
 * call from here would resume the clone somewhere inside RtlCloneUserProcess
 * itself instead of at the ORIGINAL caller of this function, and patching
 * its EIP/ESP by hand to jump back out to that caller is exactly the
 * fragile, compiler-dependent trick this design avoids.)
 *
 * NOTE: ThreadSecurityDescriptor is accepted (matching the real prototype)
 * but still has no effect: PspCreateProcess's clone branch creates the
 * child's initial thread with NULL ObjectAttributes (see the
 * PspCreateThread() call in its "This is a clone!" branch), and
 * PsCreateCloneProcess()'s parameter list has no room to plumb a thread SD
 * through to it either. That is a preexisting gap this change does not
 * close, distinct from the syscall-wiring gap it does close.
 */
NTSTATUS
NTAPI
RtlCloneUserProcess(IN ULONG ProcessFlags,
                    IN PSECURITY_DESCRIPTOR ProcessSecurityDescriptor OPTIONAL,
                    IN PSECURITY_DESCRIPTOR ThreadSecurityDescriptor OPTIONAL,
                    IN HANDLE DebugPort OPTIONAL,
                    OUT PRTL_USER_PROCESS_INFORMATION ProcessInformation)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;

    if (!ProcessInformation ||
        ProcessInformation->Size < sizeof(RTL_USER_PROCESS_INFORMATION))
    {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    RtlZeroMemory(ProcessInformation, sizeof(RTL_USER_PROCESS_INFORMATION));
    ProcessInformation->Size = sizeof(RTL_USER_PROCESS_INFORMATION);

    InitializeObjectAttributes(&ObjectAttributes,
                               NULL,
                               0,
                               NULL,
                               ProcessSecurityDescriptor);

    /*
     * SectionHandle = NULL + ParentProcess = ourselves is what tells
     * PspCreateProcess this is a clone rather than a new-image process; see
     * the comment above. This one call clones our address space and PEB
     * (real, usable memory -- PspCreateProcess's clone branch is
     * implemented) AND the calling thread's own trap context into the
     * child's initial thread, patched to return STATUS_PROCESS_CLONED, so
     * ProcessInformation is filled in with a genuinely usable clone: the
     * child, once ProcessInformation->ThreadHandle is resumed, returns from
     * THIS SAME RtlCloneUserProcess() call with STATUS_PROCESS_CLONED
     * instead of falling through here.
     */
    Status = ZwCreateProcessClone(&ProcessInformation->ProcessHandle,
                                  &ProcessInformation->ThreadHandle,
                                  &ProcessInformation->ClientId,
                                  PROCESS_ALL_ACCESS,
                                  &ObjectAttributes,
                                  NtCurrentProcess(),
                                  DebugPort);
    if (!NT_SUCCESS(Status))
    {
        /* A real failure (bad quota, out of memory, ...) */
        return Status;
    }

    /*
     * STATUS_PROCESS_CLONED (0x00000129) is an NT_SUCCESS() code, so a
     * child resuming here falls through to this same "return Status"
     * instead of the early-return above -- which IS the point: this
     * function's contract (see <ndk/rtlfuncs.h>) is that the child's copy
     * of this very call returns STATUS_PROCESS_CLONED here, not that it
     * takes some other path.
     *
     * Note ProcessInformation in the child is stale/zeroed, not populated:
     * PspCreateProcess (ntoskrnl/ps/process.c) calls MmCloneAddressSpace()
     * -- which is what snapshots the child's memory, stack included --
     * well before it captures the calling thread's context and writes the
     * CloneThreadHandle/CloneThreadClientId out-parameters back to this
     * ProcessInformation (verified from that file's line ordering: the
     * clone-address-space call happens during process-object creation, the
     * context-capture/
     * write-back only once IsClone && a real thread exists, much later in
     * the same function). So the child's stack already had ProcessInformation
     * zeroed (by RtlZeroMemory above, which also ran before the syscall)
     * at the moment its memory was snapshotted, and the later write-back
     * only ever lands in the PARENT's live memory. This matches the
     * documented contract, though: like fork(), only the parent is meant
     * to read ProcessInformation's handles; the child has no use for them.
     */
    return Status;
}

/*
 * @implemented
 */
PVOID
NTAPI
RtlEncodePointer(IN PVOID Pointer)
{
    ULONG Cookie;
    NTSTATUS Status;

    Status = ZwQueryInformationProcess(NtCurrentProcess(),
                                       ProcessCookie,
                                       &Cookie,
                                       sizeof(Cookie),
                                       NULL);
    if(!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to receive the process cookie! Status: 0x%lx\n", Status);
        return Pointer;
    }

    return (PVOID)((ULONG_PTR)Pointer ^ Cookie);
}

/*
 * @implemented
 */
PVOID
NTAPI
RtlDecodePointer(IN PVOID Pointer)
{
    return RtlEncodePointer(Pointer);
}

/*
 * @implemented
 */
PVOID
NTAPI
RtlEncodeSystemPointer(IN PVOID Pointer)
{
    return (PVOID)((ULONG_PTR)Pointer ^ SharedUserData->Cookie);
}

/*
 * @implemented
 */
PVOID
NTAPI
RtlDecodeSystemPointer(IN PVOID Pointer)
{
    return RtlEncodeSystemPointer(Pointer);
}

/*
 * @implemented
 *
 * NOTES:
 *   Implementation based on the documentation from:
 *   http://www.geoffchappell.com/studies/windows/win32/ntdll/api/rtl/peb/setprocessiscritical.htm
 */
NTSTATUS
__cdecl
RtlSetProcessIsCritical(IN BOOLEAN NewValue,
                        OUT PBOOLEAN OldValue OPTIONAL,
                        IN BOOLEAN NeedBreaks)
{
    ULONG BreakOnTermination;

    /* Initialize to FALSE */
    if (OldValue) *OldValue = FALSE;

    /* Fail, if the critical breaks flag is required but is not set */
    if ((NeedBreaks) &&
        !(NtCurrentPeb()->NtGlobalFlag & FLG_ENABLE_SYSTEM_CRIT_BREAKS))
    {
        return STATUS_UNSUCCESSFUL;
    }

    /* Check if the caller wants the old value */
    if (OldValue)
    {
        /* Query and return the old break on termination flag for the process */
        ZwQueryInformationProcess(NtCurrentProcess(),
                                  ProcessBreakOnTermination,
                                  &BreakOnTermination,
                                  sizeof(ULONG),
                                  NULL);
        *OldValue = (BOOLEAN)BreakOnTermination;
    }

    /* Set the break on termination flag for the process */
    BreakOnTermination = NewValue;
    return ZwSetInformationProcess(NtCurrentProcess(),
                                   ProcessBreakOnTermination,
                                   &BreakOnTermination,
                                   sizeof(ULONG));
}

_IRQL_requires_max_(APC_LEVEL)
ULONG
NTAPI
RtlRosGetAppcompatVersion(VOID)
{
    /* Get the current PEB */
    PPEB Peb = RtlGetCurrentPeb();
    if (Peb == NULL)
    {
        /* Default to Server 2003 */
        return _WIN32_WINNT_WS03;
    }

    /* Calculate OS version from PEB fields */
    return (Peb->OSMajorVersion << 8) | Peb->OSMinorVersion;
}
