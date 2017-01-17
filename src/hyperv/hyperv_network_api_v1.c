/*
 * hyperv_network_driver.c: network driver functions for managing
 *                          Microsoft Hyper-V host networks (API v1)
 *
 * Copyright (C) 2017 Datto Inc.
 * Copyright (C) 2011 Matthias Bolte <matthias.bolte@googlemail.com>
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
 */

#include <config.h>

#include "internal.h"
#include "virerror.h"
#include "datatypes.h"
#include "viralloc.h"
#include "viruuid.h"
#include "virstring.h"
#include "hyperv_network_api_v1.h"
#include "hyperv_wmi.h"
#include "network_conf.h"
#include "virlog.h"

#define VIR_FROM_THIS VIR_FROM_HYPERV

VIR_LOG_INIT("hyperv.hyperv_network_api_v1")

/* utility functions */
static int
hyperv1MsvmVirtualSwitchToNetwork(virConnectPtr conn,
        Msvm_VirtualSwitch *virtualSwitch, virNetworkPtr *network)
{
    unsigned char uuid[VIR_UUID_BUFLEN];
    char *rawUuid = NULL;
    int result = -1;

    if (network == NULL || *network != NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid argument"));
        return -1;
    }

    rawUuid = virtualSwitch->data->Name;

    if (virUUIDParse(rawUuid, uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not parse UUID from string '%s'"),
                       rawUuid);
        goto cleanup;
    }

    *network = virGetNetwork(conn, virtualSwitch->data->ElementName, uuid);

    if (*network == NULL) {
        goto cleanup;
    }

    result = 0;

cleanup:
    return result;
}

/* exported API functions */

int
hyperv1ConnectListNetworks(virConnectPtr conn, char **const names, int maxnames)
{
    int count = 0, i = 0;
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VirtualSwitch *switches = NULL, *entry = NULL;

    if (maxnames <= 0)
        return 0;

    virBufferAddLit(&query, MSVM_VIRTUALSWITCH_WQL_SELECT);
    virBufferAsprintf(&query, " where HealthState = 5");

    if (hypervGetMsvmVirtualSwitchList(priv, &query, &switches) < 0)
        goto cleanup;

    entry = switches;
    while (entry != NULL) {
        if (VIR_STRDUP(names[count], entry->data->ElementName) < 0)
            goto cleanup;
        count++;
        if (count >= maxnames)
            break;
        entry = entry->next;
    }

    success = true;

cleanup:
    if (!success) {
        for (i = 0; i < count; i++) {
            VIR_FREE(names[i]);
        }
        count = -1;
    }
    hypervFreeObject(priv, (hypervObject *) switches);
    virBufferFreeAndReset(&query);
    return count;
}

int
hyperv1ConnectNumOfNetworks(virConnectPtr conn)
{
    int result = -1, count = 0;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VirtualSwitch *virtualSwitchList = NULL;
    Msvm_VirtualSwitch *virtualSwitch = NULL;

    virBufferAddLit(&query, MSVM_VIRTUALSWITCH_WQL_SELECT);
    virBufferAsprintf(&query, " where HealthState = 5");
    if (hypervGetMsvmVirtualSwitchList(priv, &query, &virtualSwitchList) < 0) {
        goto cleanup;
    }

    for (virtualSwitch = virtualSwitchList; virtualSwitch != NULL;
         virtualSwitch = virtualSwitch->next) {
        count++;
    }

    result = count;

 cleanup:
    hypervFreeObject(priv, (hypervObject *) virtualSwitchList);
    virBufferFreeAndReset(&query);
    return result;
}

int
hyperv1ConnectListDefinedNetworks(virConnectPtr conn, char **const names, int maxnames)
{
    int i, count = 0;
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VirtualSwitch *virtualSwitchList = NULL;
    Msvm_VirtualSwitch *virtualSwitch = NULL;

    if (maxnames <= 0)
        return 0;

    virBufferAddLit(&query, MSVM_VIRTUALSWITCH_WQL_SELECT);
    virBufferAsprintf(&query, "where HealthState <> %d", 5);
    if (hypervGetMsvmVirtualSwitchList(priv, &query, &virtualSwitchList) < 0) {
        goto cleanup;
    }

    for (virtualSwitch = virtualSwitchList; virtualSwitch != NULL;
         virtualSwitch = virtualSwitch->next) {
        if (VIR_STRDUP(names[count], virtualSwitch->data->ElementName) < 0) {
            goto cleanup;
        }
        count++;
        if (count >= maxnames) {
            break;
        }
    }

    success = true;

 cleanup:
    if (!success) {
        for (i = 0; i < count; ++i) {
            VIR_FREE(names[i]);
        }
        count = -1;
    }

    hypervFreeObject(priv, (hypervObject *) virtualSwitchList);
    virBufferFreeAndReset(&query);

    return count;
}

virNetworkPtr
hyperv1NetworkLookupByName(virConnectPtr conn, const char *name)
{
    virNetworkPtr network = NULL;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VirtualSwitch *virtualSwitch = NULL;

    virBufferAddLit(&query, MSVM_VIRTUALSWITCH_WQL_SELECT);
    virBufferAsprintf(&query, "where Description = \"%s\" and ElementName = \"%s\"",
                      "Microsoft Virtual Switch", name);
    if (hypervGetMsvmVirtualSwitchList(priv, &query, &virtualSwitch) < 0) {
        goto cleanup;
    }
    if (virtualSwitch == NULL) {
        virReportError(VIR_ERR_NO_NETWORK,
                       _("No network found with name %s"), name);
        goto cleanup;
    }

    ignore_value(hyperv1MsvmVirtualSwitchToNetwork(conn, virtualSwitch, &network));

 cleanup:
    hypervFreeObject(priv, (hypervObject *) virtualSwitch);
    virBufferFreeAndReset(&query);

    return network;
}

int
hyperv1ConnectNumOfDefinedNetworks(virConnectPtr conn)
{
    int result = -1, count = 0;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VirtualSwitch *virtualSwitchList = NULL;
    Msvm_VirtualSwitch *virtualSwitch = NULL;

    virBufferAddLit(&query, MSVM_VIRTUALSWITCH_WQL_SELECT);
    virBufferAsprintf(&query, "where HealthState <> %d", 5);
    if (hypervGetMsvmVirtualSwitchList(priv, &query, &virtualSwitchList) < 0) {
        goto cleanup;
    }

    for (virtualSwitch = virtualSwitchList; virtualSwitch != NULL;
         virtualSwitch = virtualSwitch->next) {
        count++;
    }

    result = count;

 cleanup:
    hypervFreeObject(priv, (hypervObject *) virtualSwitchList);
    virBufferFreeAndReset(&query);

    return result;
}
