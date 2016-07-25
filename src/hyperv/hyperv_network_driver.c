/*
 * hyperv_network_driver.c: network driver functions for managing
 *                          Microsoft Hyper-V host networks
 *
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
 *
 */

#include <config.h>

#include "internal.h"
#include "virerror.h"
#include "datatypes.h"
#include "viralloc.h"
#include "viruuid.h"
#include "virstring.h"
#include "hyperv_network_driver.h"
#include "hyperv_wmi.h"
#include "network_conf.h"

#define VIR_FROM_THIS VIR_FROM_HYPERV

static virNetworkPtr
hypervNetworkLookupByName(virConnectPtr conn, const char *name)
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

    hypervMsvmVirtualSwitchToNetwork(conn, virtualSwitch, &network);

 cleanup:
    hypervFreeObject(priv, (hypervObject *) virtualSwitch);
    virBufferFreeAndReset(&query);

    return network;
}

static char *
hypervNetworkGetXMLDesc(virNetworkPtr network, unsigned int flags)
{
    char *xml = NULL;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = network->conn->privateData;
    virNetworkDefPtr def = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VirtualSwitch *virtualSwitch = NULL;

    /* Flags checked by virNetworkDefFormat */

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    virUUIDFormat(network->uuid, uuid_string);

    /* Get Msvm_VirtualSwitch */
    virBufferAddLit(&query, MSVM_VIRTUALSWITCH_WQL_SELECT);
    virBufferAsprintf(&query, "where Name = \"%s\"", uuid_string);
    if (hypervGetMsvmVirtualSwitchList(priv, &query, &virtualSwitch) < 0) {
        goto cleanup;
    }
    if (virtualSwitch == NULL) {
        virReportError(VIR_ERR_NO_NETWORK,
                       _("No network found with UUID %s"), uuid_string);
        goto cleanup;
    }

    /* Fill struct */
    if (virUUIDParse(virtualSwitch->data->Name, def->uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not parse UUID from string '%s'"),
                       virtualSwitch->data->Name);
        return NULL;
    }

    if (VIR_STRDUP(def->name, virtualSwitch->data->ElementName) < 0)
        goto cleanup;

    xml = virNetworkDefFormat(def, flags);

 cleanup:
    virNetworkDefFree(def);
    hypervFreeObject(priv, (hypervObject *)virtualSwitch);
    virBufferFreeAndReset(&query);

    return xml;
}



virNetworkDriver hypervNetworkDriver = {
    .name = "Hyper-V",
    .networkLookupByName = hypervNetworkLookupByName, /* 1.2.10 */
    .networkGetXMLDesc = hypervNetworkGetXMLDesc, /* 1.2.10 */
};


