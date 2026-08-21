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
 * @unimplemented
 *
 * Duplicates ("clones", cf. POSIX fork()) the calling process into a new
 * process, whose single initial thread starts suspended and is itself a
 * clone of the calling thread. See the header comment on the prototype in
 * <ndk/rtlfuncs.h> for the parent/child return-value contract.
 *
 * ARCHITECTURE:
 *
 * Unlike RtlCreateUserProcess() above, this does not map a new image: it
 * asks ZwCreateProcess() to clone the CALLING process's own address space
 * by passing SectionHandle = NULL together with a Parent (ourselves). That
 * is not a new code path -- it is the exact legacy fork()-emulation trick
 * real pre-Vista NT exposed via NtCreateProcess() (used by e.g. Interix and
 * Cygwin's fork()), and ReactOS's PspCreateProcess() (ntoskrnl/ps/process.c)
 * already RECOGNIZES this case ("no section handle, but a parent process
 * given" => "This is a clone!"), it just doesn't do anything about it yet:
 * both places that would need to build the child's address space and PEB
 * currently just hit `ASSERTMSG("No support for cloning yet\n", FALSE)` and
 * fall through -- which, because ASSERTMSG compiles to a no-op in a free
 * (non-debug) build, silently leaves the "cloned" process with no address
 * space content and no PEB at all. See PspCreateProcess() for the two exact
 * spots (they were changed, alongside this function, to fail cleanly with
 * STATUS_NOT_IMPLEMENTED instead of silently falling through).
 *
 * Even once that kernel-mode address-space clone exists, this function
 * still cannot correctly create the child's initial thread by itself:
 * unlike RtlCreateUserProcess() (which points a fresh thread at a fixed,
 * known entry point -- the image's TransferAddress -- via
 * RtlCreateUserThread()), a clone's child thread must resume execution at
 * the exact instruction, with the exact register and stack state, that the
 * calling thread had at the moment of THIS call -- i.e. it must look, to
 * the child, as if THIS call returned STATUS_PROCESS_CLONED instead of
 * falling through to the rest of this function. Doing that portably from
 * ntdll alone (e.g. by RtlCaptureContext() plus a hand-patched CONTEXT
 * record fed to ZwCreateThread) is fragile: nothing guarantees a plain C
 * "return Status;" a few lines below actually reads that value back out of
 * a live CPU register rather than recomputing it, so nothing here has
 * pretended to already do that. The robust way to get this right is to let
 * the KERNEL do it: the syscall that creates the process is already running
 * on top of the calling thread's own trap frame (the exact CPU state the
 * kernel saved when this thread entered kernel mode for this very call), so
 * the "clone the calling thread's continuation point" step belongs in
 * kernel mode too, sitting on top of the *already-existing*
 * PsGetContextThread() (ntoskrnl/ps/debug.c, used by NtGetContextThread /
 * Win32 GetThreadContext) to snapshot that trap frame into a CONTEXT
 * record, with only the return-value register patched to
 * STATUS_PROCESS_CLONED before it becomes the new thread's initial context.
 * That plumbing does not exist yet either -- it would most naturally live
 * as a new capability of PspCreateProcess's clone branch (so ONE syscall
 * clones the address space *and* the calling thread), rather than a second,
 * separate ZwCreateThread call from here.
 *
 * Given both kernel-mode pieces above are unimplemented, this cannot
 * succeed today. It performs the ZwCreateProcess() clone request (which
 * itself will fail once the kernel-mode change below is in place, or
 * silently misbehave without it -- see the FIXMEs in PspCreateProcess) and
 * then stops, rather than fabricate a plausible-looking but unverified
 * thread-cloning step.
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
    DPRINT1("RtlCloneUserProcess is not fully implemented -- the kernel-mode "
            "address-space and thread clone it depends on (PspCreateProcess's "
            "clone branch) is not implemented yet\n");

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
     * the comment above. InheritObjectTable = TRUE mirrors POSIX fork()'s
     * "child inherits the parent's whole descriptor/handle table" semantics.
     */
    Status = ZwCreateProcess(&ProcessInformation->ProcessHandle,
                             PROCESS_ALL_ACCESS,
                             &ObjectAttributes,
                             NtCurrentProcess(),
                             TRUE,
                             NULL,
                             DebugPort,
                             NULL);
    if (!NT_SUCCESS(Status))
    {
        /* Expected for now: see PspCreateProcess's clone branch */
        return Status;
    }

    /*
     * We have a (possibly still address-space-less, until the kernel-mode
     * FIXME above is resolved) child process object, but no portable way to
     * give it a correctly-resuming initial thread yet. Rather than create a
     * thread that would start somewhere meaningless, fail explicitly.
     */
    ZwClose(ProcessInformation->ProcessHandle);
    ProcessInformation->ProcessHandle = NULL;
    return STATUS_NOT_IMPLEMENTED;
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
