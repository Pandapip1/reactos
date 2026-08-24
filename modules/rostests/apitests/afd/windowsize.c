/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Test for AFD_INFO_RECEIVE_WINDOW_SIZE/AFD_INFO_SEND_WINDOW_SIZE
 * COPYRIGHT:   Copyright 2019 Pierre Schweitzer (pierre@reactos.org)
 */

/*
 * NOTE: this test's expectations encode *ReactOS's* AFD behaviour, not real
 * Windows's. Run against a real Windows AFD it fails 20 of its 107 tests.
 *
 * Provenance: measured on 2026-08-24 by running this very test binary on
 * Windows 11 Pro 22621, built for both i386 and x86_64. The 20 failures are
 * byte-for-byte identical on the two architectures - same line numbers, same
 * values, same order - so these are not structure-layout or pointer-width
 * problems; an x86_64 layout bug would fail on amd64 only. The sibling
 * afd apitest "send" passes 11 of 11 on both architectures, so socket
 * creation and connect are fine against real AFD; what diverges is specific
 * to AFD_INFO_RECEIVE_WINDOW_SIZE / AFD_INFO_SEND_WINDOW_SIZE.
 *
 * The two divergences:
 *
 * 1. Default window size. The checks below accept 0x1000 or 0x2000, which
 *    matches ReactOS: AfdReceiveWindowSize and AfdSendWindowSize are both
 *    0x2000 (drivers/network/afd/afd/main.c), and are copied into
 *    FCB->Recv.Size / FCB->Send.Size when the endpoint is created. Real
 *    Windows reports 65536 (0x10000) for both.
 *
 * 2. Setting the window size on an *unconnected* socket. The checks below
 *    expect STATUS_INVALID_PARAMETER; real Windows returns STATUS_SUCCESS
 *    (0). ReactOS's set path (drivers/network/afd/afd/info.c) only acts when
 *    FCB->SharedData.State == SOCKET_STATE_CONNECTED or the endpoint is
 *    AFD_ENDPOINT_CONNECTIONLESS, and otherwise takes an else branch
 *    returning STATUS_INVALID_PARAMETER.
 *
 * A second-order consequence of that same set path: it accepts a value only
 * when 0 < Ulong < 0xFFFF, so ReactOS cannot accept 65536 - real Windows's
 * own default - even on a connected socket.
 *
 * These expectations are deliberately left as they are. They correctly
 * describe ReactOS, which is this test's primary target, and rewriting them
 * to match Windows would break the test where it is actually run. Nor is
 * the driver wrong by accident: ReactOS models the window as a real
 * in-driver buffer it allocates in the set path, so honouring a set on an
 * unconnected socket would mean deferring or restructuring that allocation,
 * and raising the bound past 0xFFFF changes allocation behaviour. That is a
 * deliberately simpler model, recorded here rather than "fixed".
 */

#include "precomp.h"

static
void
TestTcp(void)
{
    NTSTATUS Status;
    HANDLE SocketHandle;
    struct sockaddr_in addr;
    ULONG OrigReceiveSize, OrigSendSize, ReceiveSize, SendSize;

    Status = AfdCreateSocket(&SocketHandle, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Status == STATUS_SUCCESS, "AfdCreateSocket failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &OrigReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(OrigReceiveSize == 0x1000 || OrigReceiveSize == 0x2000, "Invalid size: %lu\n", OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &OrigSendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(OrigSendSize == 0x1000 || OrigSendSize == 0x2000, "Invalid size: %lu\n", OrigSendSize);

    ReceiveSize = 0;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);
    SendSize = 0;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);

    ReceiveSize = (ULONG)-1L;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);
    SendSize = (ULONG)-1L;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = OrigReceiveSize;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);
    SendSize = OrigSendSize;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("0.0.0.0");
    addr.sin_port = htons(0);

    Status = AfdBind(SocketHandle, (const struct sockaddr *)&addr, sizeof(addr));
    ok(Status == STATUS_SUCCESS, "AfdBind failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = 0;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);
    SendSize = 0;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);

    ReceiveSize = (ULONG)-1L;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);
    SendSize = (ULONG)-1L;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = OrigReceiveSize;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);
    SendSize = OrigSendSize;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_INVALID_PARAMETER, "AfdSetInformation failed with %lx\n", Status);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("8.8.8.8");
    addr.sin_port = htons(53);

    Status = AfdConnect(SocketHandle, (const struct sockaddr *)&addr, sizeof(addr));
    ok(Status == STATUS_SUCCESS, "AfdConnect failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = 0;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);
    SendSize = 0;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = (ULONG)-1L;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);
    SendSize = (ULONG)-1L;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = OrigReceiveSize + 1;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);
    SendSize = OrigSendSize + 1;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = OrigReceiveSize - 1;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);
    SendSize = OrigSendSize - 1;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    NtClose(SocketHandle);
}

static
void
TestUdp(void)
{
    NTSTATUS Status;
    HANDLE SocketHandle;
    struct sockaddr_in addr;
    ULONG OrigReceiveSize, OrigSendSize, ReceiveSize, SendSize;

    Status = AfdCreateSocket(&SocketHandle, AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Status == STATUS_SUCCESS, "AfdCreateSocket failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &OrigReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(OrigReceiveSize == 0x1000 || OrigReceiveSize == 0x2000, "Invalid size: %lu\n", OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &OrigSendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(OrigSendSize == 0x1000 || OrigSendSize == 0x2000, "Invalid size: %lu\n", OrigSendSize);

    ReceiveSize = 0;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);
    SendSize = 0;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = (ULONG)-1L;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);
    SendSize = (ULONG)-1L;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = OrigReceiveSize;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);
    SendSize = OrigSendSize;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("0.0.0.0");
    addr.sin_port = htons(0);

    Status = AfdBind(SocketHandle, (const struct sockaddr *)&addr, sizeof(addr));
    ok(Status == STATUS_SUCCESS, "AfdBind failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = 0;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);
    SendSize = 0;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = (ULONG)-1L;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);
    SendSize = (ULONG)-1L;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = OrigReceiveSize + 1;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);
    SendSize = OrigSendSize + 1;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    ReceiveSize = OrigReceiveSize - 1;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);
    SendSize = OrigSendSize - 1;
    Status = AfdSetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdSetInformation failed with %lx\n", Status);

    Status = AfdGetInformation(SocketHandle, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &ReceiveSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(ReceiveSize == OrigReceiveSize, "Invalid size: %lu %lu\n", ReceiveSize, OrigReceiveSize);
    Status = AfdGetInformation(SocketHandle, AFD_INFO_SEND_WINDOW_SIZE, NULL, &SendSize, NULL);
    ok(Status == STATUS_SUCCESS, "AfdGetInformation failed with %lx\n", Status);
    ok(SendSize == OrigSendSize, "Invalid size: %lu %lu\n", SendSize, OrigSendSize);

    NtClose(SocketHandle);
}

START_TEST(windowsize)
{
    TestTcp();
    TestUdp();
}
