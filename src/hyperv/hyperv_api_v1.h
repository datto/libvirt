/*
 * hyperv_api_v1.h: core driver functions for Hyper-V API version 1 hosts
 *
 * Copyright (C) 2016 Datto Inc.
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
#ifndef __HYPERV_API_V1_H__
# define __HYPERV_API_V1_H__

#include <config.h>

#include "internal.h"
#include "datatypes.h"
#include "virdomainobjlist.h"
#include "virauth.h"
#include "viralloc.h"
#include "viruuid.h"
#include "capabilities.h"
#include "hyperv_private.h"

/* Various utility defines */
#define HYPERV1_MAX_SCSI_CONTROLLERS 4
#define HYPERV1_MAX_IDE_CONTROLLERS 2

/* Exported utility functions */
virCapsPtr hyperv1CapsInit(hypervPrivate *priv);

/* Driver functions */
const char *hyperv1ConnectGetType(virConnectPtr conn ATTRIBUTE_UNUSED);
char *hyperv1ConnectGetHostname(virConnectPtr conn);
int hyperv1ConnectGetVersion(virConnectPtr conn, unsigned long *version);
int hyperv1ConnectGetMaxVcpus(virConnectPtr conn, const char *type ATTRIBUTE_UNUSED);
int hyperv1NodeGetInfo(virConnectPtr conn, virNodeInfoPtr info);
int hyperv1ConnectListDomains(virConnectPtr conn, int *ids, int maxids);
int hyperv1ConnectNumOfDomains(virConnectPtr conn);
virDomainPtr hyperv1DomainCreateXML(virConnectPtr conn, const char *xmlDesc,
        unsigned int flags);
virDomainPtr hyperv1DomainLookupByID(virConnectPtr conn, int id);
virDomainPtr hyperv1DomainLookupByUUID(virConnectPtr conn, const unsigned char *uuid);
virDomainPtr hyperv1DomainLookupByName(virConnectPtr conn, const char *name);
virDomainPtr hyperv1DomainDefineXML(virConnectPtr conn, const char *xml);
int hyperv1DomainUndefine(virDomainPtr domain);
int hyperv1DomainUndefineFlags(virDomainPtr domain, unsigned int flags);
int hyperv1DomainSuspend(virDomainPtr domain);
int hyperv1DomainResume(virDomainPtr domain);
int hyperv1DomainShutdown(virDomainPtr domain);
int hyperv1DomainShutdownFlags(virDomainPtr domain, unsigned int flags);
int hyperv1DomainReboot(virDomainPtr domain, unsigned int flags);
int hyperv1DomainDestroyFlags(virDomainPtr domain, unsigned int flags);
int hyperv1DomainDestroy(virDomainPtr domain);
char *hyperv1DomainGetOSType(virDomainPtr domain ATTRIBUTE_UNUSED);
unsigned long long hyperv1DomainGetMaxMemory(virDomainPtr domain);
int hyperv1DomainSetMaxMemory(virDomainPtr domain, unsigned long memory);
int hyperv1DomainSetMemory(virDomainPtr domain, unsigned long memory);
int hyperv1DomainSetMemoryFlags(virDomainPtr domain, unsigned long memory,
        unsigned int flags);
int hyperv1DomainGetInfo(virDomainPtr domain, virDomainInfoPtr info);
int hyperv1DomainGetState(virDomainPtr domain, int *state, int *reason,
        unsigned int flags);
char *hyperv1DomainScreenshot(virDomainPtr domain, virStreamPtr stream,
        unsigned int screen, unsigned int flags);
int hyperv1DomainSetVcpus(virDomainPtr domain, unsigned int nvcpus);
int hyperv1DomainSetVcpusFlags(virDomainPtr domain, unsigned int nvcpus,
        unsigned int flags);
int hyperv1DomainGetVcpusFlags(virDomainPtr domain, unsigned int flags);
int hyperv1DomainGetVcpus(virDomainPtr domain, virVcpuInfoPtr info, int maxinfo,
        unsigned char *cpumaps, int maplen);
int hyperv1DomainGetMaxVcpus(virDomainPtr dom);
char *hyperv1DomainGetXMLDesc(virDomainPtr domain, unsigned int flags);
int hyperv1ConnectListDefinedDomains(virConnectPtr conn, char **const names,
        int maxnames);
int hyperv1ConnectNumOfDefinedDomains(virConnectPtr conn);
int hyperv1DomainCreateWithFlags(virDomainPtr domain, unsigned int flags);
int hyperv1DomainCreate(virDomainPtr domain);
int hyperv1DomainGetAutostart(virDomainPtr domain, int *autostart);
int hyperv1DomainSetAutostart(virDomainPtr domain, int autostart);
char *hyperv1DomainGetSchedulerType(virDomainPtr domain, int *nparams);
int hyperv1DomainGetSchedulerParameters(virDomainPtr domain,
        virTypedParameterPtr params, int *nparams);
int hyperv1DomainGetSchedulerParametersFlags(virDomainPtr domain,
        virTypedParameterPtr params, int *nparams, unsigned int flags);
int hyperv1DomainIsActive(virDomainPtr domain);
int hyperv1DomainManagedSave(virDomainPtr domain, unsigned int flags);
int hyperv1DomainHasManagedSaveImage(virDomainPtr domain, unsigned int flags);
int hyperv1DomainManagedSaveRemove(virDomainPtr domain, unsigned int flags);
int hyperv1DomainSendKey(virDomainPtr domain, unsigned int codeset,
        unsigned int holdtime, unsigned int *keycodes, int nkeycodes,
        unsigned int flags);
int hyperv1ConnectListAllDomains(virConnectPtr conn, virDomainPtr **domains,
        unsigned int flags);

#endif /* __HYPERV_API_V1_H__ */
