/*
 * hyperv_network_api_v1.h: network driver functions for managing
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

#ifndef __HYPERV_NETWORK_API_V1_H__
# define __HYPERV_NETWORK_API_V1_H__

int hyperv1ConnectListAllNetworks(virConnectPtr conn,
                                  virNetworkPtr **networks,
                                  unsigned int flags);
int hyperv1ConnectListNetworks(virConnectPtr conn, char **const names, int maxnames);
int hyperv1ConnectNumOfNetworks(virConnectPtr conn);
int hyperv1ConnectListDefinedNetworks(virConnectPtr conn, char **const names,
        int maxnames);
virNetworkPtr hyperv1NetworkLookupByName(virConnectPtr conn, const char *name);
int hyperv1ConnectNumOfDefinedNetworks(virConnectPtr conn);
char *hyperv1NetworkGetXMLDesc(virNetworkPtr network, unsigned int flags);
int hyperv1NetworkSetAutostart(virNetworkPtr network, int autostart);
int hyperv1NetworkGetAutostart(virNetworkPtr network, int *autostart);
int hyperv1NetworkIsActive(virNetworkPtr network);
int hyperv1NetworkIsPersistent(virNetworkPtr network);


#endif /* __HYPERV_NETWORK_API_V1_H__ */
