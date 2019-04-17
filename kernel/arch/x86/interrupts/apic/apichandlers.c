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
 * MollenOS x86 Advanced Programmable Interrupt Controller Driver
 * - Interrupt Handlers specific for the APIC
 */

#include <arch/utils.h>
#include <threading.h>
#include <interrupts.h>
#include <thread.h>
#include <acpi.h>
#include <apic.h>

extern size_t GlbTimerQuantum;
extern void enter_thread(Context_t *Regs);

InterruptStatus_t
ApicTimerHandler(
    _In_ FastInterruptResources_t*  NotUsed,
    _In_ void*                      Context)
{
    Context_t *Regs;
    size_t TimeSlice;
    int TaskPriority;
    UUId_t CurrCpu = ArchGetProcessorCoreId();
    _CRT_UNUSED(NotUsed);

    // Yield => start by sending eoi. It is never certain that we actually return
    // to this function due to how signals are working
    ApicSendEoi(APIC_NO_GSI, INTERRUPT_LAPIC);
    Regs = _GetNextRunnableThread((Context_t*)Context, 1, &TimeSlice, &TaskPriority);
    
    // If we are idle task - disable timer untill we get woken up
    if (!ThreadingIsCurrentTaskIdle(CurrCpu)) {
        ApicSetTaskPriority(61 - TaskPriority);
        ApicWriteLocal(APIC_INITIAL_COUNT, GlbTimerQuantum * TimeSlice);
    }
    else {
        ApicSetTaskPriority(0);
        ApicWriteLocal(APIC_INITIAL_COUNT, 0);
    }

    // Manually update interrupt status
    InterruptSetActiveStatus(0);
    enter_thread(Regs);
    return InterruptHandled;
}

InterruptStatus_t
ApicErrorHandler(
    _In_ FastInterruptResources_t*  NotUsed,
    _In_ void*                      Context)
{
    _CRT_UNUSED(Context);
    _CRT_UNUSED(NotUsed);
    return InterruptHandled;
}
