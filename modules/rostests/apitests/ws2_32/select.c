/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for select() with more sockets than FD_SETSIZE in the
 *              union of readfds, writefds and exceptfds
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 *
 * msafd's WSPSelect used to merge the three caller-supplied fd_sets into a
 * single local fd_set.  FD_SET() silently drops a socket once the set holds
 * FD_SETSIZE (64) entries, so any socket beyond the 64th distinct socket in
 * the union was never handed to AFD -- and, because the "socket not found in
 * the merged list" check for exceptfds was written as (j > HandleCount)
 * instead of (j >= HandleCount), the dropped socket also caused a one-element
 * heap buffer overflow of the AFD_POLL_INFO allocation.
 *
 * This test builds exactly that situation: FD_SETSIZE filler sockets in
 * readfds plus one further socket, present ONLY in exceptfds, that is
 * guaranteed to raise an exception (a non-blocking connect() to a port
 * nobody listens on).  If the union is truncated, that socket is not polled
 * at all and select() times out reporting nothing.
 */

#include "ws2_32.h"

/* How long we are willing to wait for the refused connect to be reported */
#define SELECT_TIMEOUT_SECONDS 10

static ULONG CheckCount = 0;
static ULONG CheckPassed = 0;
static ULONG CheckFailed = 0;

static
VOID
Check(PCSTR Name, INT Expected, INT Actual)
{
    BOOL Ok = (Expected == Actual);

    CheckCount++;
    if (Ok)
        CheckPassed++;
    else
        CheckFailed++;

    trace("CHECK %02lu [%s] %s exp=%d act=%d\n",
          CheckCount, Ok ? "PASS" : "FAIL", Name, Expected, Actual);

    ok(Ok, "CHECK %02lu %s: exp=%d act=%d\n", CheckCount, Name, Expected, Actual);
}

static
VOID
CheckSummary(VOID)
{
    trace("CHECKS EXECUTED=%lu PASSED=%lu FAILED=%lu\n",
          CheckCount, CheckPassed, CheckFailed);
}

/*
 * Reserve, then release, a loopback TCP port.  Connecting to the returned
 * port is then reliably refused, which is what makes the exceptfds socket
 * deterministically exceptional.
 */
static
USHORT
FindUnusedLoopbackPort(VOID)
{
    SOCKET Socket;
    struct sockaddr_in Address;
    int AddressLength;
    USHORT Port = 0;

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Socket == INVALID_SOCKET)
        return 0;

    RtlZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Address.sin_port = 0;

    if (bind(Socket, (struct sockaddr *)&Address, sizeof(Address)) == 0)
    {
        AddressLength = sizeof(Address);
        if (getsockname(Socket, (struct sockaddr *)&Address, &AddressLength) == 0)
            Port = Address.sin_port;
    }

    closesocket(Socket);
    return Port;
}

/*
 * Create a non-blocking socket whose connect() is refused, so that it becomes
 * exceptional (AFD_EVENT_CONNECT_FAIL -> exceptfds).
 */
static
SOCKET
CreateFailingConnectSocket(USHORT Port)
{
    SOCKET Socket;
    struct sockaddr_in Address;
    ULONG NonBlocking = 1;

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Socket == INVALID_SOCKET)
        return INVALID_SOCKET;

    if (ioctlsocket(Socket, FIONBIO, &NonBlocking) != 0)
    {
        closesocket(Socket);
        return INVALID_SOCKET;
    }

    RtlZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Address.sin_port = Port;

    /* Expected to fail with WSAEWOULDBLOCK; the failure is delivered later */
    connect(Socket, (struct sockaddr *)&Address, sizeof(Address));

    return Socket;
}

/* A bound UDP socket: valid for select(), and never readable here */
static
SOCKET
CreateFillerSocket(VOID)
{
    SOCKET Socket;
    struct sockaddr_in Address;

    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (Socket == INVALID_SOCKET)
        return INVALID_SOCKET;

    RtlZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Address.sin_port = 0;

    if (bind(Socket, (struct sockaddr *)&Address, sizeof(Address)) != 0)
    {
        closesocket(Socket);
        return INVALID_SOCKET;
    }

    return Socket;
}

/*
 * Run one select() with FillerCount filler sockets in readfds and one
 * connect-refused socket in exceptfds only.  Reports the results through
 * Check() with the supplied prefix.
 */
static
VOID
RunSelectCase(PCSTR Prefix, ULONG FillerCount, USHORT Port)
{
    SOCKET Fillers[FD_SETSIZE];
    SOCKET ExceptSocket;
    fd_set ReadSet, ExceptSet;
    struct timeval Timeout;
    ULONG i;
    ULONG Created = 0;
    int Result;
    CHAR Name[128];

    ok(FillerCount <= FD_SETSIZE, "FillerCount %lu too large\n", FillerCount);

    ExceptSocket = CreateFailingConnectSocket(Port);
    if (ExceptSocket == INVALID_SOCKET)
    {
        skip("%s: could not create the exceptfds socket\n", Prefix);
        return;
    }

    for (i = 0; i < FillerCount; i++)
    {
        Fillers[i] = CreateFillerSocket();
        if (Fillers[i] == INVALID_SOCKET)
            break;
        Created++;
    }

    if (Created != FillerCount)
    {
        skip("%s: only created %lu of %lu filler sockets\n",
             Prefix, Created, FillerCount);
        goto Cleanup;
    }

    FD_ZERO(&ReadSet);
    for (i = 0; i < FillerCount; i++)
        FD_SET(Fillers[i], &ReadSet);

    FD_ZERO(&ExceptSet);
    FD_SET(ExceptSocket, &ExceptSet);

    /* Sanity: the fillers really did all fit into one caller-side fd_set,
     * so the union select() has to build is FillerCount + 1 entries. */
    sprintf(Name, "%s.readfds_fd_count", Prefix);
    Check(Name, (INT)FillerCount, (INT)ReadSet.fd_count);
    sprintf(Name, "%s.exceptfds_fd_count", Prefix);
    Check(Name, 1, (INT)ExceptSet.fd_count);

    Timeout.tv_sec = SELECT_TIMEOUT_SECONDS;
    Timeout.tv_usec = 0;

    Result = select(0, &ReadSet, NULL, &ExceptSet, &Timeout);

    /* Exactly one socket -- the connect-refused one -- must be signalled.
     * With the truncating merge this socket was silently discarded and
     * select() returned 0 after the full timeout. */
    sprintf(Name, "%s.select_result", Prefix);
    Check(Name, 1, Result);

    sprintf(Name, "%s.exceptfds_signalled", Prefix);
    Check(Name, 1, FD_ISSET(ExceptSocket, &ExceptSet) ? 1 : 0);

    sprintf(Name, "%s.readfds_empty", Prefix);
    Check(Name, 0, (INT)ReadSet.fd_count);

Cleanup:
    for (i = 0; i < Created; i++)
        closesocket(Fillers[i]);
    closesocket(ExceptSocket);
}

START_TEST(select)
{
    WSADATA WsaData;
    USHORT Port;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        skip("WSAStartup failed\n");
        CheckSummary();
        return;
    }

    Port = FindUnusedLoopbackPort();
    if (Port == 0)
    {
        skip("Could not reserve an unused loopback port\n");
        WSACleanup();
        CheckSummary();
        return;
    }

    /* Control: a small union, well below FD_SETSIZE.  Proves that the
     * connect-refused socket really is reported through exceptfds, so that a
     * failure of the case below is about the union size and nothing else. */
    RunSelectCase("small", 1, Port);

    /* The regression case: FD_SETSIZE sockets in readfds plus one further
     * socket in exceptfds only.  Each individual fd_set is legal, but their
     * union is FD_SETSIZE + 1 entries. */
    RunSelectCase("overflow", FD_SETSIZE, Port);

    CheckSummary();
    ok(CheckFailed == 0, "%lu of %lu checks failed\n", CheckFailed, CheckCount);

    WSACleanup();
}
