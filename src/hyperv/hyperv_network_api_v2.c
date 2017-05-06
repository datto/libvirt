/*
 * hyperv_network_api_v2.c: network driver functions for managing
 *                          Microsoft Hyper-V host networks (API v2)
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
#include "hyperv_network_api_v2.h"
#include "hyperv_wmi.h"
#include "network_conf.h"
#include "virlog.h"

#define VIR_FROM_THIS VIR_FROM_HYPERV

VIR_LOG_INIT("hyperv.hyperv_network_api_v2")

/* utility functions */
static int
hyperv2MsvmVirtualSwitchToNetwork(virConnectPtr conn,
        Msvm_VirtualEthernetSwitch_V2 *virtualSwitch, virNetworkPtr *network)
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

    if (*network == NULL)
        goto cleanup;

    result = 0;

cleanup:
    return result;
}

static int
hyperv2GetVirtualSwitchList(hypervPrivate *priv,
                            const char *filter,
                            Msvm_VirtualEthernetSwitch_V2 **list)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VirtualEthernetSwitch_V2 *vSwitch = NULL;
    int count = 0;
    int ret = -1;

    virBufferAddLit(&query, MSVM_VIRTUALETHERNETSWITCH_V2_WQL_SELECT);
    virBufferAddLit(&query, " where HealthState = 5");

    /* add any caller specified WQL filter */
    if (filter)
        virBufferAsprintf(&query, " and %s", filter);

    if (hyperv2GetMsvmVirtualEthernetSwitchList(priv, &query, list) < 0)
        goto cleanup;

    for (vSwitch = *list; vSwitch != NULL; vSwitch = vSwitch->next)
        count++;

    ret = count;

 cleanup:
    virBufferFreeAndReset(&query);

    return ret;
}

/* exported API functions */
#define MATCH(FLAG) (flags & (FLAG))
int
hyperv2ConnectListAllNetworks(virConnectPtr conn,
                              virNetworkPtr **networks,
                              unsigned int flags)
{
    hypervPrivate *priv = conn->privateData;
    Msvm_VirtualEthernetSwitch_V2 *vSwitchList = NULL;
    Msvm_VirtualEthernetSwitch_V2 *vSwitch = NULL;
    virNetworkPtr network = NULL;
    virNetworkPtr *nets = NULL;
    int count = 0;
    int ret = -1;
    size_t i = 0;

    virCheckFlags(VIR_CONNECT_LIST_NETWORKS_FILTERS_ALL, -1);

    /* filter out flag options that will produce 0 results:
     * - inactive: all networks are active
     * - transient: all networks are persistend
     * - no autostart: all networks are auto start
     */
    if ((MATCH(VIR_CONNECT_LIST_NETWORKS_INACTIVE) &&
         !MATCH(VIR_CONNECT_LIST_NETWORKS_ACTIVE)) ||
        (MATCH(VIR_CONNECT_LIST_NETWORKS_TRANSIENT) &&
         !MATCH(VIR_CONNECT_LIST_NETWORKS_PERSISTENT)) ||
        (MATCH(VIR_CONNECT_LIST_DOMAINS_NO_AUTOSTART) &&
         !MATCH(VIR_CONNECT_LIST_NETWORKS_AUTOSTART))) {
        if (networks && VIR_ALLOC_N(*networks, 1) < 0)
            goto cleanup;

        ret = 0;
        goto cleanup;
    }

    count = hyperv2GetVirtualSwitchList(priv, NULL, &vSwitchList);

    if (count < 0)
        goto cleanup;

    /* if caller did not pass networks, just return the count */
    if (!networks) {
        ret = count;
        goto cleanup;
    }

    /* allocate count networks + terminating NULL element */
    if (VIR_ALLOC_N(nets, count + 1) < 0)
        goto cleanup;

    vSwitch = vSwitchList;
    for (i = 0; i < count; ++i) {
        if (hyperv2MsvmVirtualSwitchToNetwork(conn, vSwitch, &network) < 0)
            goto cleanup;

        nets[i] = network;
        network = NULL;
        vSwitch = vSwitchList->next;
    }

    ret = count;
    *networks = nets;
    nets = NULL;

 cleanup:
    if (nets) {
        for (i = 0; i < count; ++i)
            virObjectUnref(nets[i]);

        VIR_FREE(nets);
    }

    hypervFreeObject(priv, (hypervObject *) vSwitchList);

    return ret;
}
#undef MATCH

int
hyperv2ConnectListNetworks(virConnectPtr conn, char **const names, int maxnames)
{
    Msvm_VirtualEthernetSwitch_V2 *list = NULL, *entry = NULL;
    hypervPrivate *priv = conn->privateData;
    int count = 0;
    size_t i = 0;
    bool success = false;

    if (maxnames <= 0)
        return 0;

    if (hyperv2GetVirtualSwitchList(priv, NULL, &list) < 0)
        return -1;

    entry = list;
    for (i = 0; i < maxnames; ++i) {
        if (VIR_STRDUP(names[i], entry->data->ElementName) < 0)
            goto cleanup;

        count++;
        entry = entry->next;
    }

    success = true;

 cleanup:
    if (!success) {
        for (i = 0; i < count; ++i)
            VIR_FREE(names[i]);

        count = -1;
    }

    hypervFreeObject(priv, (hypervObject *) list);

    return count;
}

virNetworkPtr
hyperv2NetworkLookupByName(virConnectPtr conn, const char *name)
{
    virNetworkPtr network = NULL;
    hypervPrivate *priv = conn->privateData;
    Msvm_VirtualEthernetSwitch_V2 *virtualSwitch = NULL;
    char *filter = NULL;

    if (virAsprintf(&filter, "ElementName = \"%s\"", name) < 0)
        return NULL;

    if (hyperv2GetVirtualSwitchList(priv, filter, &virtualSwitch) < 0 ||
        virtualSwitch == NULL) {
        virReportError(VIR_ERR_NO_NETWORK,
                       _("No network found with name %s"), name);
        return NULL;
    }

    ignore_value(hyperv2MsvmVirtualSwitchToNetwork(conn, virtualSwitch, &network));

    hypervFreeObject(priv, (hypervObject *) virtualSwitch);

    return network;
}

int
hyperv2ConnectNumOfNetworks(virConnectPtr conn)
{
    Msvm_VirtualEthernetSwitch_V2 *vSwitchList = NULL;
    int count = -1;

    count = hyperv2GetVirtualSwitchList(conn->privateData, NULL, &vSwitchList);

    hypervFreeObject(conn->privateData, (hypervObject *) vSwitchList);

    return count;
}

int
hyperv2ConnectNumOfDefinedNetworks(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* Hyper-V networks are always active */
    return 0;
}

int
hyperv2ConnectListDefinedNetworks(virConnectPtr conn ATTRIBUTE_UNUSED,
                                  char **const names ATTRIBUTE_UNUSED,
                                  int maxnames ATTRIBUTE_UNUSED)
{
    /* Hyper-V networks are always active */
    return 0;
}
