/*
 * hyperv_api_v2.h: core driver functions for Hyper-V API version 2 hosts
 *
 * Copyright (C) 2017 Datto Inc.
 * Copyright (C) 2011-2013 Matthias Bolte <matthias.bolte@googlemail.com>
 * Copyright (C) 2009 Michael Sievers <msievers83@googlemail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */
#ifndef __HYPERV_API_V2_H__
# define __HYPERV_API_V2_H__

#include <config.h>

#include "internal.h"
#include "datatypes.h"
#include "virdomainobjlist.h"
#include "virauth.h"
#include "viralloc.h"
#include "viruuid.h"
#include "capabilities.h"
#include "hyperv_private.h"

#define HYPERV2_MAX_SCSI_CONTROLLERS 4
#define HYPERV2_MAX_IDE_CONTROLLERS 2

enum _Msvm_ComputerSystem_v2_EnabledState {
    MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_UNKNOWN = 0,          /* inactive */
    MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_ENABLED = 2,          /*   active */
    MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_DISABLED = 3,         /* inactive */
    MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_PAUSED = 32768,       /*   active */
    MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SUSPENDED = 32769,    /* inactive */
    MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_STARTING = 32770,     /*   active */
    MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SNAPSHOTTING = 32771, /*   active */
    MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SAVING = 32773,       /*   active */
    MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_STOPPING = 32774,     /*   active */
    MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_PAUSING = 32776,      /*   active */
    MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_RESUMING = 32777      /*   active */
};

enum _Msvm_ComputerSystem_v2_RequestedState {
    MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_ENABLED = 2,
    MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_DISABLED = 3,
    MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_PAUSED = 9,
    MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_REBOOT = 11,
    MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_SUSPENDED = 32779,
};

enum _Msvm_ConcreteJob_v2_JobState {
    MSVM_CONCRETEJOB_V2_JOBSTATE_NEW = 2,
    MSVM_CONCRETEJOB_V2_JOBSTATE_STARTING = 3,
    MSVM_CONCRETEJOB_V2_JOBSTATE_RUNNING = 4,
    MSVM_CONCRETEJOB_V2_JOBSTATE_SUSPENDED = 5,
    MSVM_CONCRETEJOB_V2_JOBSTATE_SHUTTING_DOWN = 6,
    MSVM_CONCRETEJOB_V2_JOBSTATE_COMPLETED = 7,
    MSVM_CONCRETEJOB_V2_JOBSTATE_TERMINATED = 8,
    MSVM_CONCRETEJOB_V2_JOBSTATE_KILLED = 9,
    MSVM_CONCRETEJOB_V2_JOBSTATE_EXCEPTION = 10,
    MSVM_CONCRETEJOB_V2_JOBSTATE_SERVICE = 11,
};

/* https://msdn.microsoft.com/en-us/library/hh850200(v=vs.85).aspx */
enum _Msvm_ResourceAllocationSettingData_v2_ResourceType {
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_OTHER = 1,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_IDE_CONTROLLER = 5,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_PARALLEL_SCSI_HBA = 6,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_ETHERNET_ADAPTER = 10,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_FLOPPY = 14,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_CD_DRIVE = 15,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_DVD_DRIVE = 16,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_DISK = 17,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_STORAGE_EXTENT = 19,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_SERIAL_PORT = 21,
};
/* Exported utility functions */
virCapsPtr hyperv2CapsInit(hypervPrivate *priv);

/* Driver functions */
const char *hyperv2ConnectGetType(virConnectPtr conn ATTRIBUTE_UNUSED);
char *hyperv2ConnectGetHostname(virConnectPtr conn);
int hyperv2ConnectGetVersion(virConnectPtr conn, unsigned long *version);
int hyperv2ConnectGetMaxVcpus(virConnectPtr conn, const char *type ATTRIBUTE_UNUSED);
int hyperv2NodeGetInfo(virConnectPtr conn, virNodeInfoPtr info);
int hyperv2ConnectListDomains(virConnectPtr conn, int *ids, int maxids);
int hyperv2ConnectNumOfDomains(virConnectPtr conn);
virDomainPtr hyperv2DomainCreateXML(virConnectPtr conn, const char *xmlDesc,
        unsigned int flags);
virDomainPtr hyperv2DomainLookupByID(virConnectPtr conn, int id);
virDomainPtr hyperv2DomainLookupByUUID(virConnectPtr conn, const unsigned char *uuid);
virDomainPtr hyperv2DomainLookupByName(virConnectPtr conn, const char *name);
virDomainPtr hyperv2DomainDefineXML(virConnectPtr conn, const char *xml);
int hyperv2DomainUndefine(virDomainPtr domain);
int hyperv2DomainUndefineFlags(virDomainPtr domain, unsigned int flags);
int hyperv2DomainAttachDevice(virDomainPtr domain, const char *xml);
int hyperv2DomainAttachDeviceFlags(virDomainPtr domain, const char *xml,
        unsigned int flags);
int hyperv2DomainSuspend(virDomainPtr domain);
int hyperv2DomainResume(virDomainPtr domain);
int hyperv2DomainShutdown(virDomainPtr domain);
int hyperv2DomainShutdownFlags(virDomainPtr domain, unsigned int flags);
int hyperv2DomainReboot(virDomainPtr domain, unsigned int flags);
int hyperv2DomainDestroyFlags(virDomainPtr domain, unsigned int flags);
int hyperv2DomainDestroy(virDomainPtr domain);
char *hyperv2DomainGetOSType(virDomainPtr domain ATTRIBUTE_UNUSED);
unsigned long long hyperv2DomainGetMaxMemory(virDomainPtr domain);
int hyperv2DomainSetMaxMemory(virDomainPtr domain, unsigned long memory);
int hyperv2DomainSetMemory(virDomainPtr domain, unsigned long memory);
int hyperv2DomainSetMemoryFlags(virDomainPtr domain, unsigned long memory,
        unsigned int flags);
int hyperv2DomainGetInfo(virDomainPtr domain, virDomainInfoPtr info);
int hyperv2DomainGetState(virDomainPtr domain, int *state, int *reason,
        unsigned int flags);
char *hyperv2DomainScreenshot(virDomainPtr domain, virStreamPtr stream,
        unsigned int screen, unsigned int flags);
int hyperv2DomainSetVcpus(virDomainPtr domain, unsigned int nvcpus);
int hyperv2DomainSetVcpusFlags(virDomainPtr domain, unsigned int nvcpus,
        unsigned int flags);
int hyperv2DomainGetVcpusFlags(virDomainPtr domain, unsigned int flags);
int hyperv2DomainGetVcpus(virDomainPtr domain, virVcpuInfoPtr info, int maxinfo,
        unsigned char *cpumaps, int maplen);
int hyperv2DomainGetMaxVcpus(virDomainPtr dom);
char *hyperv2DomainGetXMLDesc(virDomainPtr domain, unsigned int flags);
int hyperv2ConnectListDefinedDomains(virConnectPtr conn, char **const names,
        int maxnames);
int hyperv2ConnectNumOfDefinedDomains(virConnectPtr conn);
int hyperv2DomainCreateWithFlags(virDomainPtr domain, unsigned int flags);
int hyperv2DomainCreate(virDomainPtr domain);
int hyperv2DomainGetAutostart(virDomainPtr domain, int *autostart);
int hyperv2DomainSetAutostart(virDomainPtr domain, int autostart);
char *hyperv2DomainGetSchedulerType(virDomainPtr domain, int *nparams);
int hyperv2DomainGetSchedulerParameters(virDomainPtr domain,
        virTypedParameterPtr params, int *nparams);
int hyperv2DomainGetSchedulerParametersFlags(virDomainPtr domain,
        virTypedParameterPtr params, int *nparams, unsigned int flags);
unsigned long long hyperv2NodeGetFreeMemory(virConnectPtr conn);
int hyperv2DomainIsActive(virDomainPtr domain);
int hyperv2DomainManagedSave(virDomainPtr domain, unsigned int flags);
int hyperv2DomainHasManagedSaveImage(virDomainPtr domain, unsigned int flags);
int hyperv2DomainManagedSaveRemove(virDomainPtr domain, unsigned int flags);
int hyperv2DomainSendKey(virDomainPtr domain, unsigned int codeset,
        unsigned int holdtime, unsigned int *keycodes, int nkeycodes,
        unsigned int flags);
int hyperv2ConnectListAllDomains(virConnectPtr conn, virDomainPtr **domains,
        unsigned int flags);

#endif /* __HYPERV_API_V2_H__ */
