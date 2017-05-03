/*
 * hyperv_wmi.h: general WMI over WSMAN related functions and structures for
 *               managing Microsoft Hyper-V hosts
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

#ifndef __HYPERV_WMI_H__
# define __HYPERV_WMI_H__

# include "virbuffer.h"
# include "hyperv_private.h"
# include "hyperv_wmi_classes.h"
# include "openwsman.h"

#define ROOT_CIMV2 \
    "http://schemas.microsoft.com/wbem/wsman/1/wmi/root/cimv2/*"

#define ROOT_VIRTUALIZATION \
    "http://schemas.microsoft.com/wbem/wsman/1/wmi/root/virtualization/*"

#define ROOT_VIRTUALIZATION_V1 \
    "http://schemas.microsoft.com/wbem/wsman/1/wmi/root/virtualization/*"

#define ROOT_VIRTUALIZATION_V2 \
    "http://schemas.microsoft.com/wbem/wsman/1/wmi/root/virtualization/v2/*"

typedef struct _hypervObject hypervObject;

void hypervDebugResponseXml(WsXmlDocH response);

int hyperyVerifyResponse(WsManClient *client, WsXmlDocH response,
                         const char *detail);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * CimTypes
 */

struct cimTypes {
        const char *name;
        const char *type;
        bool isArray;
};
typedef struct cimTypes CimTypes;

struct cimClasses {
        const char *name;
        CimTypes *cimTypesPtr;
};
typedef struct cimClasses CimClasses;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Method
 */

enum _PARAM_type {
    SIMPLE_PARAM = 0,
    EPR_PARAM = 1,
    EMBEDDED_PARAM = 2
};

typedef struct _invokeXmlParam invokeXmlParam;
struct _invokeXmlParam {
    const char *name;
    int type;
    void *param;
};

typedef struct _eprParam eprParam;
struct _eprParam {
    virBufferPtr query;
    const char *wmiProviderURI;
};

typedef struct _simpleParam simpleParam;
struct _simpleParam {
    const char *value;
};

typedef struct _properties_t properties_t;
struct _properties_t {
    const char *name;
    const char *val;
};

typedef struct _embedded_param embeddedParam;
struct _embedded_param {
    const char *instanceName;
    properties_t *prop_t;
    int nbProps;
};

int hypervCreateXmlStruct(const char *methodName, const char *classURI,
        WsXmlDocH *xmlDocRoot, WsXmlNodeH *xmlNodeMethod);
int hypervAddSimpleParam(const char *paramName, const char *value,
        const char *classURI, WsXmlNodeH *parentNode);
int hypervAddEprParam(const char *paramName, virBufferPtr query,
        const char *root, const char *classURI, WsXmlNodeH *parentNode,
        WsXmlDocH doc, hypervPrivate *priv);
int hypervAddEmbeddedParam(properties_t *prop_t, int nbProps,
        const char *paramName, const char *instanceName, const char *classURI,
        WsXmlNodeH *parentNode);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Object
 */

struct _hypervObject {
    XmlSerializerInfo *serializerInfo;
    XML_TYPE_PTR data;
    hypervObject *next;
};

int hypervEnumAndPull(hypervPrivate *priv, virBufferPtr query,
                      const char *root, XmlSerializerInfo *serializerInfo,
                      const char *resourceUri, const char *className,
                      hypervObject **list);

void hypervFreeObject(hypervPrivate *priv, hypervObject *object);

const char *hypervReturnCodeToString(int returnCode);

# include "hyperv_wmi.generated.h"

#endif /* __HYPERV_WMI_H__ */
