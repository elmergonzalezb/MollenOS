/* MollenOS
 *
 * Copyright 2018, Philip Meulengracht
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
 * System Component Infrastructure 
 * - The Central Processing Unit Component is one of the primary actors
 *   in a domain. 
 */
#define __MODULE "CCPU"
#define __TRACE

#include <arch/interrupts.h>
#include <arch/thread.h>
#include <arch/utils.h>
#include <assert.h>
#include <component/domain.h>
#include <component/cpu.h>
#include <ddk/io.h>
#include <debug.h>
#include <heap.h>
#include <machine.h>
#include <string.h>

// 256 is a temporary number, once we start getting processors with more than
// 256 TXU's then we are fucked
static SystemCpuCore_t* TxuTable[256] = { 0 };
static SystemCpuCore_t  PrimaryCore   = SYSTEM_CPU_CORE_INIT;

SystemCpuCore_t*
GetProcessorCore(
    _In_ UUId_t CoreId)
{
    assert(TxuTable[CoreId] != NULL);
    return TxuTable[CoreId];
}

SystemCpuCore_t*
GetCurrentProcessorCore(void)
{
    assert(TxuTable[ArchGetProcessorCoreId()] != NULL);
    return TxuTable[ArchGetProcessorCoreId()];
}

void
InitializePrimaryProcessor(
    _In_ SystemCpu_t* Cpu)
{
    // Initialize the boot cpu's primary core pointer immediately before
    // passing it down to the arch layer - so it can use the storage.
    Cpu->Cores = &PrimaryCore;
    ArchProcessorInitialize(Cpu);
    
    // Register the primary core that has been registered
    TxuTable[Cpu->Cores->Id] = Cpu->Cores;
}

void
RegisterApplicationCore(
    _In_ SystemCpu_t*       Cpu,
    _In_ UUId_t             CoreId,
    _In_ SystemCpuState_t   InitialState,
    _In_ int                External)
{
    SystemCpuCore_t* Core;
    SystemCpuCore_t* Iter;

    assert(Cpu != NULL);
    assert(Cpu->NumberOfCores > 1);

    Core = (SystemCpuCore_t*)kmalloc(sizeof(SystemCpuCore_t));
    assert(Core != NULL);
    memset(Core, 0, sizeof(SystemCpuCore_t));

    Core->Id       = CoreId;
    Core->State    = InitialState;
    Core->External = External;
    
    // IdleThread = { 0 }
    // Scheduler  = { 0 } TODO SchedulerConstruct
    queue_construct(&Core->FunctionQueue[0]);
    queue_construct(&Core->FunctionQueue[1]);
    
    // CurrentThread      = NULL
    // InterruptRegisters = NULL
    // InterruptNesting   = 0
    // InterruptPriority  = 0
    // Link = NULL;
    
    // Add the core to the list of cores in this cpu
    Iter = Cpu->Cores;
    while (Iter->Link) {
        Iter = Iter->Link;
    }
    Iter->Link = Core;
    
    // Register the TXU in the table for quick access
    TxuTable[CoreId] = Core;
}

void
ActivateApplicationCore(
    _In_ SystemCpuCore_t* Core)
{
    SystemDomain_t*  Domain;
    SystemCpuCore_t* Iter;
    TRACE("[activate_core] %u", Core->Id);

    // Create the idle-thread and scheduler for the core
    ThreadingEnable();
    
    // Notify everyone that we are running beore switching on interrupts, don't
    // add this flag, overwrite and set only this flag
    WRITE_VOLATILE(Core->State, CpuStateRunning);
    atomic_fetch_add(&GetMachine()->NumberOfActiveCores, 1);
    InterruptEnable();

    // Bootup rest of cores in this domain if we are the primary core of
    // this domain. Then our job is simple
    Domain = GetCurrentDomain();
    if (Domain != NULL && Core == Domain->CoreGroup.Cores) {
        Iter = Domain->CoreGroup.Cores->Link;
        while (Iter) {
            StartApplicationCore(Iter);
            Iter = Iter->Link;
        }
    }

    // Enter idle loop
    WARNING("[activate_core] %" PRIuIN " is online", Core->Id);
	while (1) {
		ArchProcessorIdle();
    }
}

int
ProcessorMessageSend(
    _In_ int                     ExcludeSelf,
    _In_ SystemCpuFunctionType_t Type,
    _In_ TxuFunction_t           Function,
    _In_ void*                   Argument,
    _In_ int                     Asynchronous)
{
    SystemDomain_t*  Domain;
    SystemCpu_t*     Processor;
    SystemCpuCore_t* CurrentCore = GetCurrentProcessorCore();
    SystemCpuCore_t* Iter;
    OsStatus_t       Status;
    int              Executions = 0;
    
    assert(Function != NULL);
    
    Domain = GetCurrentDomain();
    if (Domain != NULL) {
        Processor = &Domain->CoreGroup;
    }
    else {
        Processor = &GetMachine()->Processor;
    }
    
    Iter = Processor->Cores;
    while (Iter) {
        if (ExcludeSelf && Iter == CurrentCore) {
            Iter = Iter->Link;
            continue;
        }
        
        if (READ_VOLATILE(Iter->State) & CpuStateRunning) {
            Status = TxuMessageSend(Iter->Id, Type, Function, Argument, Asynchronous);
            if (Status == OsSuccess) {
                Executions++;
            }
        }
        Iter = Iter->Link;
    }
    return Executions;
}
