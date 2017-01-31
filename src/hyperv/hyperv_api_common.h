/*
 * hyperv_api_common.h: common core driver functions for Hyper-V hosts
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

/* Exported utility functions */
virCapsPtr hyperv_GENERIC_CapsInit(hypervPrivate *priv);

/* Driver functions */
const char *hyperv_GENERIC_ConnectGetType(virConnectPtr conn ATTRIBUTE_UNUSED);
char *hyperv_GENERIC_ConnectGetHostname(virConnectPtr conn);
int hyperv_GENERIC_ConnectGetVersion(virConnectPtr conn, unsigned long *version);
int hyperv_GENERIC_ConnectGetMaxVcpus(virConnectPtr conn, const char *type ATTRIBUTE_UNUSED);
int hyperv_GENERIC_NodeGetInfo(virConnectPtr conn, virNodeInfoPtr info);
int hyperv_GENERIC_ConnectListDomains(virConnectPtr conn, int *ids, int maxids);
int hyperv_GENERIC_ConnectNumOfDomains(virConnectPtr conn);
virDomainPtr hyperv_GENERIC_DomainCreateXML(virConnectPtr conn, const char *xmlDesc,
        unsigned int flags);
virDomainPtr hyperv_GENERIC_DomainLookupByID(virConnectPtr conn, int id);
virDomainPtr hyperv_GENERIC_DomainLookupByUUID(virConnectPtr conn, const unsigned char *uuid);
virDomainPtr hyperv_GENERIC_DomainLookupByName(virConnectPtr conn, const char *name);
virDomainPtr hyperv_GENERIC_DomainDefineXML(virConnectPtr conn, const char *xml);
int hyperv_GENERIC_DomainUndefine(virDomainPtr domain);
int hyperv_GENERIC_DomainUndefineFlags(virDomainPtr domain, unsigned int flags);
int hyperv_GENERIC_DomainAttachDevice(virDomainPtr domain, const char *xml);
int hyperv_GENERIC_DomainAttachDeviceFlags(virDomainPtr domain, const char *xml,
        unsigned int flags);
int hyperv_GENERIC_DomainSuspend(virDomainPtr domain);
int hyperv_GENERIC_DomainResume(virDomainPtr domain);
int hyperv_GENERIC_DomainShutdown(virDomainPtr domain);
int hyperv_GENERIC_DomainShutdownFlags(virDomainPtr domain, unsigned int flags);
int hyperv_GENERIC_DomainReboot(virDomainPtr domain, unsigned int flags);
int hyperv_GENERIC_DomainDestroyFlags(virDomainPtr domain, unsigned int flags);
int hyperv_GENERIC_DomainDestroy(virDomainPtr domain);
char *hyperv_GENERIC_DomainGetOSType(virDomainPtr domain ATTRIBUTE_UNUSED);
unsigned long long hyperv_GENERIC_DomainGetMaxMemory(virDomainPtr domain);
int hyperv_GENERIC_DomainSetMaxMemory(virDomainPtr domain, unsigned long memory);
int hyperv_GENERIC_DomainSetMemory(virDomainPtr domain, unsigned long memory);
int hyperv_GENERIC_DomainSetMemoryFlags(virDomainPtr domain, unsigned long memory,
        unsigned int flags);
int hyperv_GENERIC_DomainGetInfo(virDomainPtr domain, virDomainInfoPtr info);
int hyperv_GENERIC_DomainGetState(virDomainPtr domain, int *state, int *reason,
        unsigned int flags);
char *hyperv_GENERIC_DomainScreenshot(virDomainPtr domain, virStreamPtr stream,
        unsigned int screen, unsigned int flags);
int hyperv_GENERIC_DomainSetVcpus(virDomainPtr domain, unsigned int nvcpus);
int hyperv_GENERIC_DomainSetVcpusFlags(virDomainPtr domain, unsigned int nvcpus,
        unsigned int flags);
int hyperv_GENERIC_DomainGetVcpusFlags(virDomainPtr domain, unsigned int flags);
int hyperv_GENERIC_DomainGetVcpus(virDomainPtr domain, virVcpuInfoPtr info, int maxinfo,
        unsigned char *cpumaps, int maplen);
int hyperv_GENERIC_DomainGetMaxVcpus(virDomainPtr dom);
char *hyperv_GENERIC_DomainGetXMLDesc(virDomainPtr domain, unsigned int flags);
int hyperv_GENERIC_ConnectListDefinedDomains(virConnectPtr conn, char **const names,
        int maxnames);
int hyperv_GENERIC_ConnectNumOfDefinedDomains(virConnectPtr conn);
int hyperv_GENERIC_DomainCreateWithFlags(virDomainPtr domain, unsigned int flags);
int hyperv_GENERIC_DomainCreate(virDomainPtr domain);
int hyperv_GENERIC_DomainGetAutostart(virDomainPtr domain, int *autostart);
int hyperv_GENERIC_DomainSetAutostart(virDomainPtr domain, int autostart);
char *hyperv_GENERIC_DomainGetSchedulerType(virDomainPtr domain, int *nparams);
int hyperv_GENERIC_DomainGetSchedulerParameters(virDomainPtr domain,
        virTypedParameterPtr params, int *nparams);
int hyperv_GENERIC_DomainGetSchedulerParametersFlags(virDomainPtr domain,
        virTypedParameterPtr params, int *nparams, unsigned int flags);
unsigned long long hyperv_GENERIC_NodeGetFreeMemory(virConnectPtr conn);
int hyperv_GENERIC_DomainIsActive(virDomainPtr domain);
int hyperv_GENERIC_DomainManagedSave(virDomainPtr domain, unsigned int flags);
int hyperv_GENERIC_DomainHasManagedSaveImage(virDomainPtr domain, unsigned int flags);
int hyperv_GENERIC_DomainManagedSaveRemove(virDomainPtr domain, unsigned int flags);
int hyperv_GENERIC_DomainSendKey(virDomainPtr domain, unsigned int codeset,
        unsigned int holdtime, unsigned int *keycodes, int nkeycodes,
        unsigned int flags);
int hyperv_GENERIC_ConnectListAllDomains(virConnectPtr conn, virDomainPtr **domains,
        unsigned int flags);

