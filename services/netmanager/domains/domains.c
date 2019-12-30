/**
 * MollenOS
 *
 * Copyright 2019, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Network Manager (Domain Handler)
 * - Contains the implementation of the socket domain type in the network
 *   manager. There a lot of different types of sockets, like internet, ipc
 *   and bluetooth to name the popular ones.
 */

#include "domains.h"
#include <inet/socket.h>
#include "../socket.h"

typedef struct SocketDomain {
    SocketDomainOps_t Ops;
} SocketDomain_t;

// Supported domains
extern OsStatus_t DomainUnspecCreate(SocketDomain_t**);
extern OsStatus_t DomainLocalCreate(SocketDomain_t**);
extern OsStatus_t DomainInternetCreate(int, SocketDomain_t**);
extern OsStatus_t DomainBluetoothCreate(SocketDomain_t**);

OsStatus_t
DomainCreate(
    _In_ int              DomainType,
    _In_ SocketDomain_t** DomainOut)
{
    switch (DomainType) {
        case AF_UNSPEC: {
            return DomainUnspecCreate(DomainOut);
        } break;
        case AF_LOCAL: {
            return DomainLocalCreate(DomainOut);
        } break;
        case AF_INET:
        case AF_INET6: {
            return DomainInternetCreate(DomainType, DomainOut);
        } break;
        case AF_BLUETOOTH: {
            return DomainBluetoothCreate(DomainOut);
        } break;
        
        default:
            break;
    }
    return OsInvalidParameters;
}

void
DomainDestroy(
    _In_ SocketDomain_t* Domain)
{
    if (!Domain) {
        return;
    }
    Domain->Ops.Destroy(Domain);
}

OsStatus_t
DomainAllocateAddress(
    _In_ Socket_t* Socket)
{
    if (!Socket->Domain) {
        return OsInvalidParameters;
    }
    return Socket->Domain->Ops.AddressAllocate(Socket);
}

OsStatus_t
DomainUpdateAddress(
    _In_ Socket_t*              Socket,
    _In_ const struct sockaddr* Address)
{
    if (!Socket->Domain) {
        return OsInvalidParameters;
    }
    return Socket->Domain->Ops.Bind(Socket, Address);
}

void
DomainFreeAddress(
    _In_ Socket_t* Socket)
{
    if (!Socket->Domain) {
        return;
    }
    Socket->Domain->Ops.AddressFree(Socket);
}

OsStatus_t
DomainConnect(
    _In_ thrd_t                 Waiter,
    _In_ Socket_t*              Socket,
    _In_ const struct sockaddr* Address)
{
    if (!Socket->Domain) {
        return OsInvalidParameters;
    }
    return Socket->Domain->Ops.Connect(Waiter, Socket, Address);
}

OsStatus_t
DomainDisconnect(
    _In_ Socket_t* Socket)
{
    if (!Socket->Domain) {
        return OsInvalidParameters;
    }
    return Socket->Domain->Ops.Disconnect(Socket);
}

OsStatus_t
DomainAccept(
    _In_ UUId_t    ProcessHandle,
    _In_ thrd_t    Waiter,
    _In_ Socket_t* Socket)
{
    if (!Socket->Domain) {
        return OsInvalidParameters;
    }
    return Socket->Domain->Ops.Accept(ProcessHandle, Waiter, Socket);
}

OsStatus_t
DomainPair(
    _In_ Socket_t* Socket1,
    _In_ Socket_t* Socket2)
{
    if (!Socket1->Domain || !Socket2->Domain) {
        return OsInvalidParameters;
    }
    return Socket1->Domain->Ops.Pair(Socket1, Socket2);
}

OsStatus_t
DomainSend(
    _In_ Socket_t* Socket)
{
    if (!Socket->Domain) {
        return OsInvalidParameters;
    }
    return Socket->Domain->Ops.Send(Socket);
}

OsStatus_t
DomainReceive(
    _In_ Socket_t* Socket)
{
    if (!Socket->Domain) {
        return OsInvalidParameters;
    }
    return Socket->Domain->Ops.Receive(Socket);
}

OsStatus_t
DomainGetAddress(
    _In_ Socket_t*        Socket,
    _In_ int              Source,
    _In_ struct sockaddr* Address)
{
    if (!Socket->Domain) {
        return OsInvalidParameters;
    }
    return Socket->Domain->Ops.GetAddress(Socket, Source, Address);
}
