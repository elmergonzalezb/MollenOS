/* MollenOS
*
* Copyright 2011 - 2016, Philip Meulengracht
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
* MollenOS Architecture Header
*/

#ifndef _MCORE_MAIN_ARCH_
#define _MCORE_MAIN_ARCH_

/* Includes */
#include <os/osdefs.h>

/* The definition of a thread id
* used for identifying threads */
typedef void(*ThreadEntry_t)(void*);

/* Select Correct ARCH file */
#if defined(_X86_32)
#include "x86\x32\Arch.h"
#elif defined(_X86_64)
#include "x86\x64\Arch.h"
#else
#error "Unsupported Architecture :("
#endif

/* Typedef this */
typedef Registers_t Context_t;

/* These must be implemented by 
 * the underlying architecture */

/***********************
* Address Spaces       *
* Used for abstracting *
* the virtual memory   *
***********************/

/* This is the how many bits per register
* definition, used by the memory bitmap */
#if defined(_X86_32)
#define MEMORY_BITS					32
#define MEMORY_LIMIT				0xFFFFFFFF
#define MEMORY_MASK_DEFAULT			0xFFFFFFFF
#elif defined(_X86_64)
#define MEMORY_BITS					64
#define MEMORY_LIMIT				0xFFFFFFFFFFFFFFFF
#define MEMORY_MASK_DEFAULT			0xFFFFFFFFFFFFFFFF
#else
#error "Unsupported Architecture :("
#endif

/* Address Space Flags */
#define ADDRESS_SPACE_KERNEL		0x1
#define ADDRESS_SPACE_INHERIT		0x2
#define ADDRESS_SPACE_USER			0x4

/* Allocation Flags */
#define ADDRESS_SPACE_FLAG_USER			0x1
#define ADDRESS_SPACE_FLAG_RESERVE		0x2
#define ADDRESS_SPACE_FLAG_NOCACHE		0x4
#define ADDRESS_SPACE_FLAG_VIRTUAL		0x8

__CRT_EXTERN void AddressSpaceInitKernel(AddressSpace_t *Kernel);
__CRT_EXTERN AddressSpace_t *AddressSpaceCreate(int Flags);
__CRT_EXTERN void AddressSpaceDestroy(AddressSpace_t *AddrSpace);
__CRT_EXTERN void AddressSpaceSwitch(AddressSpace_t *AddrSpace);
_CRT_EXPORT AddressSpace_t *AddressSpaceGetCurrent(void);

__CRT_EXTERN void AddressSpaceReleaseKernel(AddressSpace_t *AddrSpace);
_CRT_EXPORT Addr_t AddressSpaceMap(AddressSpace_t *AddrSpace, 
	VirtAddr_t Address, size_t Size, Addr_t Mask, int Flags);
_CRT_EXPORT void AddressSpaceMapFixed(AddressSpace_t *AddrSpace,
	PhysAddr_t PhysicalAddr, VirtAddr_t VirtualAddr, size_t Size, int Flags);
_CRT_EXPORT void AddressSpaceUnmap(AddressSpace_t *AddrSpace, VirtAddr_t Address, size_t Size);
_CRT_EXPORT PhysAddr_t AddressSpaceGetMap(AddressSpace_t *AddrSpace, VirtAddr_t Address);

/************************
 * Threading            *
 * Used for abstracting *
 * arch specific thread *
 ************************/
#include <threading.h>

/* IThreadCreate
 * Initializes a new x86-specific thread context
 * for the given threading flags, also initializes
 * the yield interrupt handler first time its called */
__CRT_EXTERN void *IThreadCreate(Flags_t ThreadFlags, Addr_t EntryPoint);

/* IThreadSetupUserMode
 * Initializes user-mode data for the given thread, and
 * allocates all neccessary resources (x86 specific) for
 * usermode operations */
__CRT_EXTERN void IThreadSetupUserMode(MCoreThread_t *Thread, 
	Addr_t StackAddress, Addr_t EntryPoint, Addr_t ArgumentAddress);

/* IThreadDestroy
 * Free's all the allocated resources for x86
 * specific threading operations */
__CRT_EXTERN void IThreadDestroy(MCoreThread_t *Thread);

/* IThreadWakeCpu
 * Wake's the target cpu from an idle thread
 * by sending it an yield IPI */
__CRT_EXTERN void IThreadWakeCpu(Cpu_t Cpu);

/* IThreadYield
 * Yields the current thread control to the scheduler */
__CRT_EXTERN void IThreadYield(void);

/************************
 * Device Io Spaces     *
 * Used for abstracting *
 * device addressing    *
 ************************/
#include <os/driver/io.h>

/* Represents an io-space in MollenOS, they represent
 * some kind of communication between hardware and software
 * by either port or mmio */
typedef struct _MCoreIoSpace {
	IoSpaceId_t			Id;
	PhxId_t				Owner;
	int					Type;
	Addr_t				PhysicalBase;
	Addr_t				VirtualBase;
	size_t				Size;
} MCoreIoSpace_t;

/* Initialize the Io Space manager so we 
 * can register io-spaces from drivers and the
 * bus code */
__CRT_EXTERN void IoSpaceInitialize(void);

/* Registers an io-space with the io space manager 
 * and assigns the io-space a unique id for later
 * identification */
__CRT_EXTERN OsStatus_t IoSpaceRegister(DeviceIoSpace_t *IoSpace);

/* Acquires the given memory space by mapping it in
 * the current drivers memory space if needed, and sets
 * a lock on the io-space */
__CRT_EXTERN OsStatus_t IoSpaceAcquire(DeviceIoSpace_t *IoSpace);

/* Releases the given memory space by unmapping it from
 * the current drivers memory space if needed, and releases
 * the lock on the io-space */
__CRT_EXTERN OsStatus_t IoSpaceRelease(DeviceIoSpace_t *IoSpace);

/* Destroys the given io-space by its id, the id
 * has the be valid, and the target io-space HAS to 
 * un-acquired by any process, otherwise its not possible */
__CRT_EXTERN OsStatus_t IoSpaceDestroy(IoSpaceId_t IoSpace);

/* Tries to validate the given virtual address by 
 * checking if any process has an active io-space
 * that involves that virtual address */
__CRT_EXTERN Addr_t IoSpaceValidate(Addr_t Address);

/***********************
* Device Interface     *
***********************/
__CRT_EXTERN int DeviceAllocateInterrupt(void *mCoreDevice);

#endif