/* MollenOS
*
* Copyright 2011 - 2014, Philip Meulengracht
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
* MollenOS X86 Devices Interfaces
*
*/

/* Includes */
#include <Modules/ModuleManager.h>
#include <AcpiSys.h>
#include <Pci.h>
#include <Heap.h>
#include <List.h>
#include <Timers.h>
#include <Log.h>

/* Definitions */
#define DEVICES_LEGACY_ID		0x0000015A
#define DEVICES_ACPI_ID			0x0000AC71

#define DEVICES_HPET			0x00000008

#define DEVICES_CMOS			0x00000008
#define DEVICES_PS2				0x00000010
#define DEVICES_PIT				0x00000018
#define DEVICES_RTC				0x00000020

/* Extern the Pci Device List */
extern list_t *GlbPciDevices;
extern void rdtsc(uint64_t *Value);

/* Helpers */
uint32_t CreateModuleClass(uint8_t PciClass, uint8_t PciSubClass)
{
	return (uint32_t)(0 | (PciClass << 16) | PciSubClass);
}

uint32_t CreateModuleSubClass(uint8_t Interface, uint8_t Protocol)
{
	return (uint32_t)(0 | (Interface << 16) | Protocol);
}

/* This enumerates EHCI controllers and makes sure all routing goes to
* their companion controllers */
void DevicesDisableEHCI(void *Data, int n)
{
	/* Vars */
	PciDevice_t *Driver = (PciDevice_t*)Data;
	list_t *SubBusList;

	/* Needed for loading */
	MCoreModule_t *Module = NULL;

	/* Unused */
	_CRT_UNUSED(n);

	/* Check type */
	switch (Driver->Type)
	{
	case X86_PCI_TYPE_BRIDGE:
	{
		/* Sanity */
		if (Driver->Children != NULL)
		{
			/* Get bus list */
			SubBusList = (list_t*)Driver->Children;

			/* Iterate Deeper */
			list_execute_all(SubBusList, DevicesDisableEHCI);
		}

	} break;

	case X86_PCI_TYPE_DEVICE:
	{
		/* Get driver */

		/* Serial Bus Comms */
		if (Driver->Header->Class == 0x0C)
		{
			/* Usb? */
			if (Driver->Header->Subclass == 0x03)
			{
				/* Controller Type? */

				/* UHCI -> 0. OHCI -> 0x10. EHCI -> 0x20. xHCI -> 0x30 */
				if (Driver->Header->Interface == 0x20)
				{
					/* Initialise Controller */
					Module = ModuleFind(0x000C0003, 0x00200000);

					/* Do we have the driver? */
					if (Module != NULL)
						ModuleLoad(Module, Data);
				}
			}
		}

	} break;

	default:
		break;
	}
}

/* This installs a driver for each device present (if we have a driver!) */
void DevicesInstall(void *Data, int n)
{
	/* Vars */
	PciDevice_t *PciDev = (PciDevice_t*)Data;
	list_t *SubBus;

	/* Needed for loading */
	MCoreModule_t *Module = NULL;

	/* We dont really use 'n' */
	_CRT_UNUSED(n);

	switch (PciDev->Type)
	{
	case X86_PCI_TYPE_BRIDGE:
	{
		/* Get bus list */
		SubBus = (list_t*)PciDev->Children;

		/* Install drivers on that bus */
		list_execute_all(SubBus, DevicesInstall);

	} break;

	case X86_PCI_TYPE_DEVICE:
	{
		/* Sanity, We ignore EHCI */
		if (PciDev->Header->Class == 0x0C
			&& PciDev->Header->Subclass == 0x03
			&& PciDev->Header->Interface == 0x20)
		{
			break;
		}

		/* Initialise Device */
		Module = ModuleFind(
			CreateModuleClass(PciDev->Header->Class, PciDev->Header->Subclass), 
			CreateModuleSubClass(PciDev->Header->Interface, 0));

		/* Do we have the driver? */
		if (Module != NULL)
			ModuleLoad(Module, Data);

	} break;

	default:
		break;
	}
}

/* Initialises all available timers in system */
void DevicesInitTimers(void)
{
	/* Vars */
	MCoreModule_t *Module = NULL;
	ACPI_TABLE_HEADER *Header = NULL;

	/* Information */
	LogInformation("TIMR", "Initializing System Timers");

	/* Step 1. Load the CMOS Clock */
	Module = ModuleFind(DEVICES_LEGACY_ID, DEVICES_CMOS);

	/* Do we have the driver? */
	if (Module != NULL)
		ModuleLoad(Module, &AcpiGbl_FADT.Century);

	/* Step 2. Try to setup HPET 
	 * I'd rather ignore the rest */
	if (ACPI_SUCCESS(AcpiGetTable(ACPI_SIG_HPET, 0, &Header)))
	{
		/* There is hope, we have Hpet header */
		Module = ModuleFind(DEVICES_ACPI_ID, DEVICES_HPET);

		/* Do we have the driver? */
		if (Module != NULL)
		{
			/* Cross fingers for the Hpet driver */
			if (ModuleLoad(Module, (void*)Header) == ModuleOk)
				return;
		}
	}

	/* Damn.. 
	 * Step 3. Initialize the PIT */
	Module = ModuleFind(DEVICES_LEGACY_ID, DEVICES_PIT);

	/* Do we have the driver? */
	if (Module != NULL)
	{
		/* Great, load driver */
		if (ModuleLoad(Module, NULL) == ModuleOk)
			return;
	}
		
	/* Wtf? No PIT? 
	 * Step 4. Last resort to the rtc-clock */
	Module = ModuleFind(DEVICES_LEGACY_ID, DEVICES_RTC);

	/* Do we have the driver? */
	if (Module != NULL)
		ModuleLoad(Module, NULL);
}

/* Initialises all available devices in system */
void DevicesInit(void *Args)
{
	/* Vars */
	MCoreModule_t *Module = NULL;

	/* Unused */
	_CRT_UNUSED(Args);

	/* Enumerate Pci Space */
	PciEnumerate();

	/* Now, setup drivers
	 * since we have no EHCI
	 * driver, but still would
	 * like usb functionality,
	 * we must make sure we disable these */
	list_execute_all(GlbPciDevices, DevicesDisableEHCI);

	/* Setup the rest */
	list_execute_all(GlbPciDevices, DevicesInstall);

	/* Setup Legacy Devices, those
	* PciEnumerate does not detect */

	/* PS2 */
	Module = ModuleFind(DEVICES_LEGACY_ID, DEVICES_PS2);

	/* Do we have the driver? */
	if (Module != NULL)
		ModuleLoad(Module, NULL);
}

/* Externs */
extern x86CpuObject_t GlbBootCpuInfo;

/* Backup Timer, Should always be provided */
void DelayMs(uint32_t MilliSeconds)
{
	/* Keep value in this */
	uint64_t Counter = 0;
	volatile uint64_t TimeOut = 0;

	/* Sanity */
	if (!(GlbBootCpuInfo.EdxFeatures & CPUID_FEAT_EDX_TSC))
	{
		LogFatal("TIMR", "DelayMs() was called, but no TSC support in CPU.");
		Idle();
	}

	/* Use rdtsc for this */
	rdtsc(&Counter);
	TimeOut = Counter + (uint64_t)(MilliSeconds * 100000);

	while (Counter < TimeOut) { rdtsc(&Counter); }
}