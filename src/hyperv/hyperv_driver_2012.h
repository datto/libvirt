/*
 * hyperv_driver.h: core driver functions for managing Microsoft Hyper-V hosts
 *
 * Copyright (C) 2011 Matthias Bolte <matthias.bolte@googlemail.com>
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

#ifndef __HYPERV_DRIVER_2012_H__
# define __HYPERV_DRIVER_2012_H__

#include <config.h>

#include "internal.h"
#include "datatypes.h"
#include "virdomainobjlist.h"
#include "virauth.h"
#include "viralloc.h"
#include "virlog.h"
#include "viruuid.h"
#include "hyperv_driver.h"
#include "hyperv_network_driver.h"
#include "hyperv_private.h"
#include "hyperv_util.h"
#include "hyperv_wmi.h"
#include "openwsman.h"
#include "virstring.h"
#include "virtypedparam.h"

#define VIR_FROM_THIS VIR_FROM_HYPERV

int
hypervConnectListAllDomains2012(virConnectPtr conn,
                            virDomainPtr **domains,
                            unsigned int flags);

int
hypervDomainGetState2012(virDomainPtr domain, int *state, int *reason,
                     unsigned int flags);

int
hypervDomainCreate2012(virDomainPtr domain);

int
hypervDomainCreateWithFlags2012(virDomainPtr domain, unsigned int flags);

virDomainPtr
hypervDomainLookupByID2012(virConnectPtr conn, int id);

virDomainPtr
hypervDomainLookupByUUID2012(virConnectPtr conn, const unsigned char *uuid);

virDomainPtr
hypervDomainLookupByName2012(virConnectPtr conn, const char *name);

int
hypervDomainShutdownFlags2012(virDomainPtr domain, unsigned int flags);

int
hypervDomainShutdown2012(virDomainPtr dom);

int
hypervDomainDestroyFlags2012(virDomainPtr domain, unsigned int flags);

int
hypervDomainDestroy2012(virDomainPtr domain);

int
hypervDomainReboot2012(virDomainPtr domain, unsigned int flags);

int
hypervDomainIsActive2012(virDomainPtr domain);

int
hypervDomainUndefineFlags2012(virDomainPtr domain, 
                              unsigned int flags ATTRIBUTE_UNUSED);

int
hypervDomainUndefine2012(virDomainPtr domain);

char *
hypervDomainGetXMLDesc2012(virDomainPtr domain, unsigned int flags);

int
hypervConnectNumOfDomains2012(virConnectPtr conn);

int
hypervConnectListDomains2012(virConnectPtr conn, int *ids, int maxids);

int
hypervConnectNumOfDefinedDomains2012(virConnectPtr conn);

int
hypervConnectListDefinedDomains2012(virConnectPtr conn, char **const names, 
                                    int maxnames);

virDomainPtr
hypervDomainDefineXML2012(virConnectPtr conn, const char *xml);

int
hypervDomainGetInfo2012(virDomainPtr domain, virDomainInfoPtr info);

int
hypervDomainSetMemoryFlags2012(virDomainPtr domain, unsigned long memory,
                               unsigned int flags ATTRIBUTE_UNUSED);

int
hypervDomainSetMemory2012(virDomainPtr domain, unsigned long memory);

int
hypervDomainSendKey2012(virDomainPtr domain,
                    unsigned int codeset,
                    unsigned int holdtime,
                    unsigned int *keycodes,
                    int nkeycodes,
                    unsigned int flags);

#endif /* __HYPERV_DRIVER_2012_H__ */
