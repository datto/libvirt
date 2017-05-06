/*
 * hyperv_network_api_v2.h: network driver functions for managing
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

#ifndef __HYPERV_NETWORK_API_V2_H__
# define __HYPERV_NETWORK_API_V2_H__


int hyperv2ConnectListAllNetworks(virConnectPtr conn,
                                  virNetworkPtr **networks,
                                  unsigned int flags);
int hyperv2ConnectListNetworks(virConnectPtr conn, char **const names, int maxnames);
int hyperv2ConnectNumOfNetworks(virConnectPtr conn);
int hyperv2ConnectListDefinedNetworks(virConnectPtr conn, char **const names,
        int maxnames);
virNetworkPtr hyperv2NetworkLookupByName(virConnectPtr conn, const char *name);
int hyperv2ConnectNumOfDefinedNetworks(virConnectPtr conn);


#endif /* __HYPERV_NETWORK_API_V2_H__ */
