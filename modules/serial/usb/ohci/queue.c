/* MollenOS
 *
 * Copyright 2011 - 2017, Philip Meulengracht
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
 * MollenOS MCore - Open Host Controller Interface Driver
 * TODO:
 *	- Power Management
 */
//#define __TRACE

/* Includes 
 * - System */
#include <os/mollenos.h>
#include <os/utils.h>
#include "ohci.h"

/* Includes
 * - Library */
#include <ds/list.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* Aligns address (with roundup if alignment is set) */
uintptr_t OhciAlign(uintptr_t Address, uintptr_t AlignmentBitMask, uintptr_t Alignment)
{
	uintptr_t AlignedAddr = Address;

	if (AlignedAddr & AlignmentBitMask)
	{
		AlignedAddr &= ~AlignmentBitMask;
		AlignedAddr += Alignment;
	}

	return AlignedAddr;
}

/* OhciQueueInitialize
 * Initialize the controller's queue resources and resets counters */
OsStatus_t
OhciQueueInitialize(
	_In_ OhciController_t *Controller)
{
	// Variables
	OhciGTransferDescriptor_t *NullTd = NULL;
	OhciControl_t *Queue = &Controller->QueueControl;
	uintptr_t PoolPhysical = 0, NullPhysical = 0;
	size_t PoolSize;
	void *Pool = NULL;
	int i;

	// Trace
	TRACE("OhciQueueInitialize()");

	// Null out queue-control
	memset(Queue, 0, sizeof(OhciControl_t));

	// Calculate how many bytes of memory we will need
	PoolSize = (OHCI_POOL_EDS + 32) * sizeof(OhciEndpointDescriptor_t);
	PoolSize += OHCI_POOL_TDS * sizeof(OhciGTransferDescriptor_t);

	// Allocate both TD's and ED's pool
	if (MemoryAllocate(PoolSize, MEMORY_CLEAN | MEMORY_COMMIT
		| MEMORY_LOWFIRST | MEMORY_CONTIGIOUS, &Pool, &PoolPhysical) != OsSuccess) {
		ERROR("Failed to allocate memory for resource-pool");
		return OsError;
	}

	// Initialize pointers
	Queue->EDPool = (OhciEndpointDescriptor_t**)Pool;
	Queue->EDPoolPhysical = PoolPhysical;
	Queue->TDPool = (OhciGTransferDescriptor_t**)((uint8_t*)Pool +
		(OHCI_POOL_EDS + 32) * sizeof(OhciEndpointDescriptor_t));
	Queue->TDPoolPhysical = PoolPhysical + 
		(OHCI_POOL_EDS + 32) * sizeof(OhciEndpointDescriptor_t);

	// Initialize the null-td
	NullTd = Queue->TDPool[OHCI_POOL_TDNULL];
	NullTd->BufferEnd = 0;
	NullTd->Cbp = 0;
	NullTd->Link = 0x0;
	NullTd->Flags = 0;
	NullPhysical = OHCI_POOL_TDINDEX(Queue->TDPoolPhysical, OHCI_POOL_TDNULL);

	// Enumerate the ED pool and set their links
	// to the NULL descriptor
	for (i = 0; i < (OHCI_POOL_EDS + 32); i++) {
		// Mark it skippable and set a NULL td
		Queue->EDPool[i]->Flags = OHCI_EP_SKIP;
		Queue->EDPool[i]->Current =
			(Queue->EDPool[i]->TailPointer = NullPhysical) | 0x1;
	}

	// Initialize transaction counters
	// and the transaction list
	Queue->TransactionList = ListCreate(KeyInteger, LIST_SAFE);
	return OsSuccess;
}

/* OhciQueueDestroy
 * Unschedules any scheduled ed's and frees all resources allocated
 * by the initialize function */
OsStatus_t
OhciQueueDestroy(
	_In_ OhciController_t *Controller)
{

}

/* OhciVisualizeQueue
 * Visualizes (by text..) the current interrupt table queue 
 * by going through all 32 base nodes and their links */
void
OhciVisualizeQueue(
	_In_ OhciController_t *Controller)
{
	// Variables
	int i;

	// Enumerate the 32 entries
	for (i = 0; i < 32; i++) {
		OhciEndpointDescriptor_t *Ed = 
			Controller->QueueControl.EDPool[OHCI_POOL_EDS + i];

		// Enumerate links
		while (Ed) {
			// TRACE
			TRACE("0x%x -> ", (EpPtr->Flags & OHCI_EP_SKIP));
			Ed = (OhciEndpointDescriptor_t*)Ed->LinkVirtual;
		}
	}
}

/* OhciEdAllocate
 * Allocates a new ED for usage with the transaction. If this returns
 * NULL we are out of ED's and we should wait till next transfer. */
OhciEndpointDescriptor_t*
OhciEdAllocate(
	_In_ OhciController_t *Controller, 
	_In_ UsbTransferType_t Type)
{
	/* Vars */
	OhciEndpointDescriptor_t *Ed = NULL;
	int i;

	/* Pick a QH */
	SpinlockAcquire(&Controller->Lock);

	/* Grap it, locked operation */
	if (Type == ControlTransfer
		|| Type == BulkTransfer)
	{
		/* Grap Index */
		for (i = 0; i < OHCI_POOL_NUM_ED; i++)
		{
			/* Sanity */
			if (Controller->EDPool[i]->HcdFlags & OHCI_ED_ALLOCATED)
				continue;

			/* Yay!! */
			Controller->EDPool[i]->HcdFlags = OHCI_ED_ALLOCATED;
			Ed = Controller->EDPool[i];
			break;
		}

		/* Sanity */
		if (i == 50)
			kernel_panic("USB_OHCI::WTF RAN OUT OF EDS\n");
	}
	else if (Type == InterruptTransfer
		|| Type == IsochronousTransfer)
	{
		/* Allocate */
		Addr_t aSpace = (Addr_t)kmalloc(sizeof(OhciEndpointDescriptor_t) + OHCI_STRUCT_ALIGN);
		Ed = (OhciEndpointDescriptor_t*)OhciAlign(aSpace, OHCI_STRUCT_ALIGN_BITS, OHCI_STRUCT_ALIGN);

		/* Zero it out */
		memset(Ed, 0, sizeof(OhciEndpointDescriptor_t));
	}

	/* Release Lock */
	SpinlockRelease(&Controller->Lock);

	/* Done! */
	return Ed;
}

/* Td Functions */
Addr_t OhciAllocateTd(OhciEndpoint_t *Ep, UsbTransferType_t Type)
{
	/* Vars */
	OhciGTransferDescriptor_t *Td;
	Addr_t cIndex = 0;
	size_t i;

	/* Pick a QH */
	SpinlockAcquire(&Ep->Lock);

	/* Sanity */
	if (Type == ControlTransfer
		|| Type == BulkTransfer)
	{
		/* Grap it, locked operation */
		for (i = 0; i < Ep->TdsAllocated; i++)
		{
			/* Sanity */
			if (Ep->TDPool[i]->Flags & OHCI_TD_ALLOCATED)
				continue;

			/* Yay!! */
			Ep->TDPool[i]->Flags |= OHCI_TD_ALLOCATED;
			cIndex = (Addr_t)i;
			break;
		}

		/* Sanity */
		if (i == Ep->TdsAllocated)
			kernel_panic("USB_OHCI::WTF ran out of TD's!!!!\n");
	}
	else if (Type == InterruptTransfer)
	{
		/* Allocate a new */
		Td = (OhciGTransferDescriptor_t*)OhciAlign(((Addr_t)kmalloc(sizeof(OhciGTransferDescriptor_t) + OHCI_STRUCT_ALIGN)), OHCI_STRUCT_ALIGN_BITS, OHCI_STRUCT_ALIGN);

		/* Null it */
		memset((void*)Td, 0, sizeof(OhciGTransferDescriptor_t));

		/* Set as index */
		cIndex = (Addr_t)Td;
	}
	else
	{
		/* Allocate iDescriptor */
		OhciITransferDescriptor_t *iTd = (OhciITransferDescriptor_t*)OhciAlign(((Addr_t)kmalloc(sizeof(OhciITransferDescriptor_t) + OHCI_STRUCT_ALIGN)), OHCI_STRUCT_ALIGN_BITS, OHCI_STRUCT_ALIGN);

		/* Null it */
		memset((void*)iTd, 0, sizeof(OhciITransferDescriptor_t));

		/* Set as index */
		cIndex = (Addr_t)iTd;
	}

	/* Release Lock */
	SpinlockRelease(&Ep->Lock);

	return cIndex;
}

/* OhciEdInitialize
 * Initializes and sets up the endpoint descriptor with 
 * the given values */
void
OhciEdInitialize(
	_Out_ OhciEndpointDescriptor_t *Ed, 
	_In_ uintptr_t HeadPhysical, 
	_In_ UsbTransferType_t Type,
	_In_ size_t Address, 
	_In_ size_t Endpoint, 
	_In_ size_t PacketSize,
	_In_ UsbSpeed_t Speed)
{
	/* Set Head & Tail Td */
	if (HeadPhysical == OHCI_LINK_END) {
		Ed->Current = OHCI_LINK_END;
		Ed->TailPointer = 0;
	}
	else
	{
		/* Vars */
		Addr_t FirstTdAddr = (Addr_t)FirstTd->TransferDescriptor;
		Addr_t LastTd = 0;

		/* Get tail */
		UsbHcTransaction_t *FirstLink = FirstTd;
		while (FirstLink->Link)
			FirstLink = FirstLink->Link;
		LastTd = (Addr_t)FirstLink->TransferDescriptor;

		/* Get physical addresses */
		Ed->TailPtr = AddressSpaceGetMap(AddressSpaceGetCurrent(), (Addr_t)LastTd);
		Ed->HeadPtr = AddressSpaceGetMap(AddressSpaceGetCurrent(), (Addr_t)FirstTdAddr) | OHCI_LINK_END;
	}

	// Initialize flags
	Ed->Flags = OHCI_EP_SKIP;
	Ed->Flags |= (Address & OHCI_EP_ADDRESS_MASK);
	Ed->Flags |= OHCI_EP_ENDPOINT(Endpoint);
	Ed->Flags |= OHCI_EP_INOUT_TD; /* Get PID from Td */
	Ed->Flags |= OHCP_EP_LOWSPEED((Speed == LowSpeed) ? 1 : 0);
	Ed->Flags |= OHCI_EP_MAXLEN(PacketSize);
	Ed->Flags |= OHCI_EP_TYPE(Type);

	// If it's isochronous add a special flag to indicate
	// the type of td's used
	if (Type == IsochronousTransfer) {
		Ed->Flags |= OHCI_EP_ISOCHRONOUS;
	}
}

/* Setup TD */
OhciGTransferDescriptor_t *OhciTdSetup(OhciEndpoint_t *Ep, UsbTransferType_t Type,
	UsbPacket_t *pPacket, void **TDBuffer)
{
	/* Vars */
	OhciGTransferDescriptor_t *Td;
	Addr_t TDIndex;
	void *Buffer;

	/* Allocate a Td */
	TDIndex = OhciAllocateTd(Ep, Type);

	/* Grab a Td and a Buffer */
	Td = Ep->TDPool[TDIndex];
	Buffer = Ep->TDPoolBuffers[TDIndex];

	/* Set no link */
	Td->NextTD = OHCI_LINK_END;

	/* Setup the Td for a SETUP Td */
	Td->Flags = OHCI_TD_ALLOCATED;
	Td->Flags |= OHCI_TD_PID_SETUP;
	Td->Flags |= OHCI_TD_NO_IOC;
	Td->Flags |= OHCI_TD_TOGGLE_LOCAL;
	Td->Flags |= OHCI_TD_ACTIVE;

	/* Setup the SETUP Request */
	*TDBuffer = Buffer;
	memcpy(Buffer, (void*)pPacket, sizeof(UsbPacket_t));

	/* Set Td Buffer */
	Td->Cbp = AddressSpaceGetMap(AddressSpaceGetCurrent(), (VirtAddr_t)Buffer);
	Td->BufferEnd = Td->Cbp + sizeof(UsbPacket_t) - 1;

	return Td;
}

OhciGTransferDescriptor_t *OhciTdIo(OhciEndpoint_t *OhciEp, UsbTransferType_t Type,
	UsbHcEndpoint_t *Endpoint, uint32_t PId, size_t Length, void **TDBuffer)
{
	/* Vars */
	OhciGTransferDescriptor_t *Td = NULL;
	OhciITransferDescriptor_t *iTd = NULL;
	Addr_t TDIndex;
	void *Buffer;

	/* Allocate a Td */
	TDIndex = OhciAllocateTd(OhciEp, Type);

	/* Sanity */
	if (Type == ControlTransfer || Type == BulkTransfer) {
		Td = OhciEp->TDPool[TDIndex];
		Buffer = OhciEp->TDPoolBuffers[TDIndex];
	}
	else if (Type == InterruptTransfer) {
		Td = (OhciGTransferDescriptor_t*)TDIndex;
		Buffer = (void*)kmalloc_a(PAGE_SIZE);
	}
	else
	{
		/* Calculate frame count - Maximum packet size is 1023 bytes */
		uint32_t FrameCount = DIVUP(Length, 1023);
		uint32_t BufItr = 0;
		uint32_t FrameItr = 0;
		uint32_t Crossed = 0;

		/* If direction is out and mod 1023 is 0
		* add a zero-length frame */

		/* Cast */
		iTd = (OhciITransferDescriptor_t*)TDIndex;

		/* Allocate a buffer */
		Buffer = (void*)kmalloc_a(Length);

		/* IF framecount is > 8, nono */
		if (FrameCount > 8)
			FrameCount = 8;

		/* Setup */
		iTd->Flags = 0;
		iTd->Flags |= OHCI_TD_FRAMECOUNT(FrameCount - 1);
		iTd->Flags |= OHCI_TD_NO_IOC;

		/* Buffer */
		iTd->Bp0 = AddressSpaceGetMap(AddressSpaceGetCurrent(), (VirtAddr_t)Buffer);
		iTd->BufferEnd = iTd->Bp0 + Length - 1;

		/* Setup offsets */
		while (FrameCount)
		{
			/* Set offset 0 */
			iTd->Offsets[FrameItr] = (BufItr & 0xFFF);
			iTd->Offsets[FrameItr] = ((Crossed & 0x1) << 12);

			/* Increase buffer */
			BufItr += 1023;

			/* Sanity */
			if (BufItr >= PAGE_SIZE)
			{
				/* Reduce, set crossed */
				BufItr -= PAGE_SIZE;
				Crossed = 1;
			}

			/* Set iterators */
			FrameItr++;
			FrameCount--;
		}

		/* EOL */
		iTd->NextTD = OHCI_LINK_END;

		/* Done */
		return (OhciGTransferDescriptor_t*)iTd;
	}

	/* EOL */
	Td->NextTD = OHCI_LINK_END;

	/* Setup the Td for a IO Td */
	Td->Flags = OHCI_TD_ALLOCATED;
	Td->Flags |= PId;
	Td->Flags |= OHCI_TD_NO_IOC;
	Td->Flags |= OHCI_TD_TOGGLE_LOCAL;
	Td->Flags |= OHCI_TD_ACTIVE;

	/* Allow short packet? */
	if (Type == ControlTransfer) {
		if (PId == OHCI_TD_PID_IN && Length > 0)
			Td->Flags |= OHCI_TD_SHORTPACKET;
	}
	else if (PId == OHCI_TD_PID_IN)
		Td->Flags |= OHCI_TD_SHORTPACKET;

	/* Toggle? */
	if (Endpoint->Toggle)
		Td->Flags |= OHCI_TD_TOGGLE;

	/* Store buffer */
	*TDBuffer = Buffer;

	/* Bytes to transfer?? */
	if (Length > 0) {
		Td->Cbp = AddressSpaceGetMap(AddressSpaceGetCurrent(), (VirtAddr_t)Buffer);
		Td->BufferEnd = Td->Cbp + Length - 1;
	}
	else {
		Td->Cbp = 0;
		Td->BufferEnd = 0;
	}

	/* Done! */
	return Td;
}

/* OhciCalculateQueue
 * Calculates the queue the ed should get linked to by analyzing the
 * current bandwidth load, and the requested load. Returns -1 on error */
int
OhciCalculateQueue(
	_In_ OhciController_t *Controller, 
	_In_ size_t Interval, 
	_In_ size_t Bandwidth)
{
	// Variables
	OhciControl_t *Queue = &Controller->QueueControl;
	int	Index = -1;
	size_t i;

	// iso periods can be huge; iso tds specify frame numbers
	if (Interval > OHCI_BANDWIDTH_PHASES) {
		Interval = OHCI_BANDWIDTH_PHASES;
	}

	// Find the least loaded queue
	for (i = 0; i < Interval; i++) {
		if (Index < 0 || Queue->Bandwidth[Index] > Queue->Bandwidth[i]) {
			int	j;

			// Usb 1.1 says 90% of one frame must be isoc or intr
			for (j = i; j < OHCI_BANDWIDTH_PHASES; j += Interval) {
				if ((Queue->Bandwidth[j] + Bandwidth) > 900)
					break;
			}

			// Sanity bounds of j
			if (j < OHCI_BANDWIDTH_PHASES) {
				continue;
			}

			// Update queue index
			Index = i;
		}
	}

	// Return found index
	return Index;
}

/* Linking Functions */
OsStatus_t
OhciLinkGeneric(
	_In_ OhciController_t *Controller, 
	_In_ UsbHcRequest_t *Request)
{
	// Variables
	OhciControl_t *Queue = &Controller->QueueControl;
	OhciEndpointDescriptor_t *Ep = (OhciEndpointDescriptor_t*)Request->Data;
	Addr_t EdAddress = 0;

	/* Get physical */
	EdAddress = AddressSpaceGetMap(AddressSpaceGetCurrent(), (VirtAddr_t)Ep);

	if (Request->Type == ControlTransfer) {
		if (Queue->TransactionsWaitingControl > 0) {
			// Insert into front if 0
			if (Queue->TransactionQueueControl == 0)
				Queue->TransactionQueueControl = (uint32_t)Request->Data;
			else
			{
				OhciEndpointDescriptor_t *Ed = 
					(OhciEndpointDescriptor_t*)Queue->TransactionQueueControl;

				// Iterate until end of chain
				while (Ed->Link) {
					Ed = (OhciEndpointDescriptor_t*)Ed->LinkVirtual;
				}

				// Insert it
				Ed->Link = EdAddress;
				Ed->LinkVirtual = (reg32_t)Request->Data;
			}

			/* Increase */
			Queue->TransactionsWaitingControl++;
		}
		else
		{
			// Add it HcControl/BulkCurrentED
			Controller->Registers->HcControlHeadED =
				Controller->Registers->HcControlCurrentED = EdAddress;
			Queue->TransactionsWaitingControl++;

			// Enable control list
			Controller->Registers->HcCommandStatus |= OHCI_COMMAND_CONTROL_ACTIVE;
		}
	}
	else if (Request->Type == BulkTransfer) {
		if (Queue->TransactionsWaitingBulk > 0) {
			// Insert into front if 0
			if (Queue->TransactionQueueBulk == 0) {
				Queue->TransactionQueueBulk = (uintptr_t)Request->Data;
			}
			else {
				OhciEndpointDescriptor_t *Ed = 
					(OhciEndpointDescriptor_t*)Queue->TransactionQueueBulk;

				// Iterate until end of chain
				while (Ed->Link) {
					Ed = (OhciEndpointDescriptor_t*)Ed->LinkVirtual;
				}

				// Insert it
				Ed->Link = EdAddress;
				Ed->LinkVirtual = (reg32_t)Request->Data;
			}

			Queue->TransactionsWaitingBulk++;
		}
		else {
			// Add it HcControl/BulkCurrentED
			Controller->Registers->HcBulkHeadED =
				Controller->Registers->HcBulkCurrentED = EdAddress;
			Queue->TransactionsWaitingBulk++;
			
			// Enable bulk list
			Controller->Registers->HcCommandStatus |= OHCI_COMMAND_BULK_ACTIVE;
		}
	}
}

void OhciLinkPeriodic(OhciController_t *Controller, UsbHcRequest_t *Request)
{
	/* Variables */
	OhciEndpointDescriptor_t *Ep = (OhciEndpointDescriptor_t*)Request->Data;
	Addr_t EdAddress = 0;
	int Queue = 0;
	int i;

	/* Get physical */
	EdAddress = AddressSpaceGetMap(AddressSpaceGetCurrent(), (VirtAddr_t)Ep);

	/* Find queue for this ED */
	Queue = OhciCalculateQueue(Controller, Request->Endpoint->Interval, Ep->Bandwidth);

	/* Sanity */
	assert(Queue >= 0);

	for (i = Queue; i < OHCI_BANDWIDTH_PHASES; i += (int)Ep->Interval)
	{
		/* Vars */
		OhciEndpointDescriptor_t **PrevEd = &Controller->ED32[i];
		OhciEndpointDescriptor_t *Here = *PrevEd;
		uint32_t *PrevPtr = (uint32_t*)&Controller->HCCA->InterruptTable[i];

		/* sorting each branch by period (slow before fast)
		* lets us share the faster parts of the tree.
		* (plus maybe: put interrupt eds before iso)
		*/
		while (Here && Ep != Here) {
			if (Ep->Interval > Here->Interval)
				break;
			PrevEd = &((OhciEndpointDescriptor_t*)Here->NextEDVirtual);
			PrevPtr = &Here->NextED;
			Here = *PrevEd;
		}

		/* Sanity */
		if (Ep != Here) {
			Ep->NextEDVirtual = (uint32_t)Here;
			if (Here)
				Ep->NextED = *PrevPtr;

			/* MemB */
			MemoryBarrier();

			/* Update */
			*PrevEd = Ep;
			*PrevPtr = EdAddress;

			/* MemB */
			MemoryBarrier();
		}

		/* Increase */
		Controller->Bandwidth[i] += Ep->Bandwidth;
	}

	/* Store ed info */
	Ep->HcdFlags |= OHCI_ED_SET_QUEUE(Queue);
}

/* Unlinking Functions */
void OhciUnlinkPeriodic(OhciController_t *Controller, UsbHcRequest_t *Request)
{
	/* Cast */
	OhciEndpointDescriptor_t *Ed = (OhciEndpointDescriptor_t*)Request->Data;
	int Queue = OHCI_ED_GET_QUEUE(Ed->HcdFlags);
	int i;

	/* Iterate queues */
	for (i = Queue; i < OHCI_BANDWIDTH_PHASES; i += (int)Ed->Interval)
	{
		/* Vars */
		OhciEndpointDescriptor_t *Temp;
		OhciEndpointDescriptor_t **PrevEd = &Controller->ED32[i];
		uint32_t *PrevPtr = (uint32_t*)&Controller->HCCA->InterruptTable[i];

		/* Iterate til we find it */
		while (*PrevEd && (Temp = *PrevEd) != Ed) {
			PrevPtr = &Temp->NextED;
			PrevEd = &((OhciEndpointDescriptor_t*)Temp->NextEDVirtual);
		}

		/* Sanity */
		if (*PrevEd) {
			*PrevPtr = Ed->NextED;
			*PrevEd = (OhciEndpointDescriptor_t*)Ed->NextEDVirtual;
		}

		/* Decrease Bandwidth */
		Controller->Bandwidth[i] -= Ed->Bandwidth;
	}
}

/* Reload Controller */
void OhciReloadControlBulk(OhciController_t *Controller, UsbTransferType_t TransferType)
{
	/* So now, before waking up a sleeper we see if Transactions are pending
	* if they are, we simply copy the queue over to the current */

	/* Any Controls waiting? */
	if (TransferType == ControlTransfer)
	{
		if (Controller->TransactionsWaitingControl > 0)
		{
			/* Get physical of Ed */
			Addr_t EdPhysical = AddressSpaceGetMap(AddressSpaceGetCurrent(), Controller->TransactionQueueControl);

			/* Set it */
			Controller->Registers->HcControlHeadED =
				Controller->Registers->HcControlCurrentED = EdPhysical;

			/* Start queue */
			Controller->Registers->HcCommandStatus |= OHCI_COMMAND_CONTROL_ACTIVE;
		}

		/* Reset control queue */
		Controller->TransactionQueueControl = 0;
		Controller->TransactionsWaitingControl = 0;
	}
	else if (TransferType == BulkTransfer)
	{
		/* Bulk */
		if (Controller->TransactionsWaitingBulk > 0)
		{
			/* Get physical of Ed */
			Addr_t EdPhysical = AddressSpaceGetMap(AddressSpaceGetCurrent(), Controller->TransactionQueueBulk);

			/* Add it to queue */
			Controller->Registers->HcBulkHeadED =
				Controller->Registers->HcBulkCurrentED = EdPhysical;

			/* Start queue */
			Controller->Registers->HcCommandStatus |= OHCI_COMMAND_BULK_ACTIVE;
		}

		/* Reset control queue */
		Controller->TransactionQueueBulk = 0;
		Controller->TransactionsWaitingBulk = 0;
	}
}

/* Process Done Queue */
void OhciProcessDoneQueue(OhciController_t *Controller, Addr_t DoneHeadAddr)
{
	/* Get transaction list */
	List_t *Transactions = (List_t*)Controller->TransactionList;

	/* Get Ed with the same td address as DoneHeadAddr */
	foreach(Node, Transactions)
	{
		/* Cast UsbRequest */
		UsbHcRequest_t *HcRequest = (UsbHcRequest_t*)Node->Data;

		/* Get Ed */
		OhciEndpointDescriptor_t *Ed = (OhciEndpointDescriptor_t*)HcRequest->Data;
		UsbTransferType_t TransferType = (UsbTransferType_t)((Ed->Flags >> 27) & 0xF);

		/* Get transaction list */
		UsbHcTransaction_t *tList = (UsbHcTransaction_t*)HcRequest->Transactions;

		/* Is it this? */
		while (tList)
		{
			/* Get physical of TD */
			Addr_t TdPhysical = AddressSpaceGetMap(AddressSpaceGetCurrent(), 
				(VirtAddr_t)tList->TransferDescriptor);

			/* Is it this one? */
			if (DoneHeadAddr == TdPhysical)
			{
				/* Depending on type */
				if (TransferType == ControlTransfer
					|| TransferType == BulkTransfer)
				{
					/* Reload */
					OhciReloadControlBulk(Controller, TransferType);

					/* Wake a node */
					SchedulerWakeupOneThread((Addr_t*)Ed);

					/* Remove from list */
					ListRemoveByNode(Transactions, Node);

					/* Cleanup node */
					kfree(Node);
				}
				else if (TransferType == InterruptTransfer
					|| TransferType == IsochronousTransfer)
				{
					/* Re-Iterate */
					UsbHcTransaction_t *lIterator = HcRequest->Transactions;
					int SwitchToggles = HcRequest->TransactionCount % 2;
					int ErrorTransfer = 0;

					/* Copy data if not dummy */
					while (lIterator)
					{
						/* Get Td */
						OhciGTransferDescriptor_t *Td =
							(OhciGTransferDescriptor_t*)lIterator->TransferDescriptor;
						
						/* Get condition-code */
						int ConditionCode = OHCI_TD_GET_CC(Td->Flags);

						/* Sanity */
						if ((ConditionCode != 0 
								&& ConditionCode != 15)
							|| ErrorTransfer) {
							ErrorTransfer = 1;
						}
						else
						{
							/* Let's see 
							 * Only copy data */
							if (lIterator->Length != 0
								&& Td->Flags & OHCI_TD_PID_IN)
								memcpy(lIterator->Buffer, lIterator->TransferBuffer, lIterator->Length);

							/* Switch toggle */
							if (TransferType == InterruptTransfer
								&& SwitchToggles)
							{
								OhciGTransferDescriptor_t *__Td =
									(OhciGTransferDescriptor_t*)lIterator->TransferDescriptorCopy;

								/* Clear Toggle */
								__Td->Flags &= ~OHCI_TD_TOGGLE;

								/* Set it? */
								if (HcRequest->Endpoint->Toggle)
									__Td->Flags |= OHCI_TD_TOGGLE;

								/* Switch toggle bit */
								HcRequest->Endpoint->Toggle =
									(HcRequest->Endpoint->Toggle == 1) ? 0 : 1;
							}

							/* Restart Td */
							memcpy(lIterator->TransferDescriptor,
								lIterator->TransferDescriptorCopy,
								TransferType == InterruptTransfer ?
								sizeof(OhciGTransferDescriptor_t) : sizeof(OhciITransferDescriptor_t));
						}

						/* Eh, next link */
						lIterator = lIterator->Link;
					}

					/* Callback */
					if (HcRequest->Callback != NULL)
						HcRequest->Callback->Callback(HcRequest->Callback->Args, 
							ErrorTransfer == 1 ? TransferStalled : TransferFinished);

					/* Restore data for 
					 * out transfers */
					lIterator = HcRequest->Transactions;
					while (lIterator)
					{
						/* Get Td */
						OhciGTransferDescriptor_t *Td =
							(OhciGTransferDescriptor_t*)lIterator->TransferDescriptor;

						/* Let's see
						* Only copy data */
						if (lIterator->Length != 0
							&& Td->Flags & OHCI_TD_PID_OUT)
							memcpy(lIterator->TransferBuffer, lIterator->Buffer, lIterator->Length);

						/* Eh, next link */
						lIterator = lIterator->Link;
					}

					/* Restart Ed */
					if (!ErrorTransfer)
						Ed->HeadPtr = 
							AddressSpaceGetMap(AddressSpaceGetCurrent(), 
								(VirtAddr_t)HcRequest->Transactions->TransferDescriptor);
				}

				/* Done */
				return;
			}

			/* Go to next */
			tList = tList->Link;
		}
	}
}

/* Process Transactions 
 * This code unlinks / links pending endpoint descriptors */
void OhciProcessTransactions(OhciController_t *Controller)
{
	/* Get transaction list */
	List_t *Transactions = (List_t*)Controller->TransactionList;

	/* Iterate list */
	foreach(Node, Transactions)
	{
		/* Cast UsbRequest */
		UsbHcRequest_t *HcRequest = (UsbHcRequest_t*)Node->Data;

		/* Get Ed */
		OhciEndpointDescriptor_t *Ed = (OhciEndpointDescriptor_t*)HcRequest->Data;

		/* Has this Ed requested linkage? */
		if (Ed->HcdFlags & OHCI_ED_SCHEDULE)
		{
			/* What kind of scheduling is requested? */
			if (HcRequest->Type == ControlTransfer
				|| HcRequest->Type == BulkTransfer) {
				/* Link */
				OhciLinkGeneric(Controller, HcRequest);
			}
			else {
				/* Link */
				OhciLinkPeriodic(Controller, HcRequest);

				/* Make sure periodic list is active */
				Controller->Registers->HcControl |= OHCI_CONTROL_PERIODIC_ACTIVE | OHCI_CONTROL_ISOC_ACTIVE;
			}

			/* Remove scheduling flag */
			Ed->HcdFlags &= ~OHCI_ED_SCHEDULE;
		}
		else if (Ed->HcdFlags & OHCI_ED_UNSCHEDULE)
		{
			/* Only interrupt and isoc requests unscheduling */
			OhciUnlinkPeriodic(Controller, HcRequest);

			/* Remove unscheduling flag */
			Ed->HcdFlags &= ~OHCI_ED_UNSCHEDULE;

			/* Wake up process if anyone was waiting for us to unlink */
			SchedulerWakeupOneThread((Addr_t*)HcRequest->Data);
		}
	}
}