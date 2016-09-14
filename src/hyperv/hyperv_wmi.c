/*
 * hyperv_wmi.c: general WMI over WSMAN related functions and structures for
 *               managing Microsoft Hyper-V hosts
 *
 * Copyright (C) 2014 Red Hat, Inc.
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

#include <config.h>
#include <wsman-soap.h>  /* Where struct _WsXmlDoc is defined (necessary to dereference WsXmlDocH type) */

#include "internal.h"
#include "virerror.h"
#include "datatypes.h"
#include "viralloc.h"
#include "viruuid.h"
#include "virbuffer.h"
#include "hyperv_private.h"
#include "hyperv_wmi.h"
#include "virstring.h"
#include "hyperv_wmi_cimtypes.generated.h"

#define WS_SERIALIZER_FREE_MEM_WORKS 0

#define VIR_FROM_THIS VIR_FROM_HYPERV

/* This function guarantees that query is freed, even on failure */
int
hyperyVerifyResponse(WsManClient *client, WsXmlDocH response,
                     const char *detail)
{
    int lastError = wsmc_get_last_error(client);
    int responseCode = wsmc_get_response_code(client);
    WsManFault *fault;

    if (lastError != WS_LASTERR_OK) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Transport error during %s: %s (%d)"),
                       detail, wsman_transport_get_last_error_string(lastError),
                       lastError);
        return -1;
    }

    /* Check the HTTP response code and report an error if it's not 200 (OK),
     * 400 (Bad Request) or 500 (Internal Server Error) */
    if (responseCode != 200 && responseCode != 400 && responseCode != 500) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected HTTP response during %s: %d"),
                       detail, responseCode);
        return -1;
    }

    if (response == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Empty response during %s"), detail);
        return -1;
    }

    if (wsmc_check_for_fault(response)) {
        fault = wsmc_fault_new();

        if (fault == NULL) {
            virReportOOMError();
            return -1;
        }

        wsmc_get_fault_data(response, fault);

        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("SOAP fault during %s: code '%s', subcode '%s', "
                         "reason '%s', detail '%s'"),
                       detail, NULLSTR(fault->code), NULLSTR(fault->subcode),
                       NULLSTR(fault->reason), NULLSTR(fault->fault_detail));

        wsmc_fault_destroy(fault);
        return -1;
    }

    return 0;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Object
 */

/* This function guarantees that query is freed, even on failure */
int
hypervEnumAndPull(hypervPrivate *priv, virBufferPtr query, const char *root,
                  XmlSerializerInfo *serializerInfo, const char *resourceUri,
                  const char *className, hypervObject **list)
{
    int result = -1;
    WsSerializerContextH serializerContext;
    client_opt_t *options = NULL;
    char *query_string = NULL;
    filter_t *filter = NULL;
    WsXmlDocH response = NULL;
    char *enumContext = NULL;
    hypervObject *head = NULL;
    hypervObject *tail = NULL;
    WsXmlNodeH node = NULL;
    XML_TYPE_PTR data = NULL;
    hypervObject *object;

    if (virBufferCheckError(query) < 0) {
        virBufferFreeAndReset(query);
        return -1;
    }
    query_string = virBufferContentAndReset(query);

    if (list == NULL || *list != NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid argument"));
        VIR_FREE(query_string);
        return -1;
    }

    serializerContext = wsmc_get_serialization_context(priv->client);

    options = wsmc_options_init();

    if (options == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not initialize options"));
        goto cleanup;
    }

    filter = filter_create_simple(WSM_WQL_FILTER_DIALECT, query_string);

    if (filter == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not create filter"));
        goto cleanup;
    }

    response = wsmc_action_enumerate(priv->client, root, options, filter);

    if (hyperyVerifyResponse(priv->client, response, "enumeration") < 0)
        goto cleanup;

    enumContext = wsmc_get_enum_context(response);

    ws_xml_destroy_doc(response);
    response = NULL;

    while (enumContext != NULL && *enumContext != '\0') {
        response = wsmc_action_pull(priv->client, resourceUri, options,
                                    filter, enumContext);

        if (hyperyVerifyResponse(priv->client, response, "pull") < 0)
            goto cleanup;

        node = ws_xml_get_soap_body(response);

        if (node == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Could not lookup SOAP body"));
            goto cleanup;
        }

        node = ws_xml_get_child(node, 0, XML_NS_ENUMERATION, WSENUM_PULL_RESP);

        if (node == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Could not lookup pull response"));
            goto cleanup;
        }

        node = ws_xml_get_child(node, 0, XML_NS_ENUMERATION, WSENUM_ITEMS);

        if (node == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Could not lookup pull response items"));
            goto cleanup;
        }

        if (ws_xml_get_child(node, 0, resourceUri, className) == NULL)
            break;

        data = ws_deserialize(serializerContext, node, serializerInfo,
                              className, resourceUri, NULL, 0, 0);

        if (data == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Could not deserialize pull response item"));
            goto cleanup;
        }

        if (VIR_ALLOC(object) < 0)
            goto cleanup;

        object->serializerInfo = serializerInfo;
        object->data = data;

        data = NULL;

        if (head == NULL) {
            head = object;
        } else {
            tail->next = object;
        }

        tail = object;

        VIR_FREE(enumContext);
        enumContext = wsmc_get_enum_context(response);

        ws_xml_destroy_doc(response);
        response = NULL;
    }

    *list = head;
    head = NULL;

    result = 0;

 cleanup:
    if (options != NULL)
        wsmc_options_destroy(options);

    if (filter != NULL)
        filter_destroy(filter);

    if (data != NULL) {
#if WS_SERIALIZER_FREE_MEM_WORKS
        /* FIXME: ws_serializer_free_mem is broken in openwsman <= 2.2.6,
         *        see hypervFreeObject for a detailed explanation. */
        if (ws_serializer_free_mem(serializerContext, data,
                                   serializerInfo) < 0) {
            VIR_ERROR(_("Could not free deserialized data"));
        }
#endif
    }

    VIR_FREE(query_string);
    ws_xml_destroy_doc(response);
    VIR_FREE(enumContext);
    hypervFreeObject(priv, head);

    return result;
}

void
hypervFreeObject(hypervPrivate *priv ATTRIBUTE_UNUSED, hypervObject *object)
{
    hypervObject *next;
#if WS_SERIALIZER_FREE_MEM_WORKS
    WsSerializerContextH serializerContext;
#endif

    if (object == NULL)
        return;

#if WS_SERIALIZER_FREE_MEM_WORKS
    serializerContext = wsmc_get_serialization_context(priv->client);
#endif

    while (object != NULL) {
        next = object->next;

#if WS_SERIALIZER_FREE_MEM_WORKS
        /* FIXME: ws_serializer_free_mem is broken in openwsman <= 2.2.6,
         *        but this is not that critical, because openwsman keeps
         *        track of all allocations of the deserializer and frees
         *        them in wsmc_release. So this doesn't result in a real
         *        memory leak, but just in piling up unused memory until
         *        the connection is closed. */
        if (ws_serializer_free_mem(serializerContext, object->data,
                                   object->serializerInfo) < 0) {
            VIR_ERROR(_("Could not free deserialized data"));
        }
#endif

        VIR_FREE(object);

        object = next;
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * CIM/Msvm_ReturnCode
 */

const char *
hypervReturnCodeToString(int returnCode)
{
    switch (returnCode) {
      case CIM_RETURNCODE_COMPLETED_WITH_NO_ERROR:
        return _("Completed with no error");

      case CIM_RETURNCODE_NOT_SUPPORTED:
        return _("Not supported");

      case CIM_RETURNCODE_UNKNOWN_ERROR:
        return _("Unknown error");

      case CIM_RETURNCODE_CANNOT_COMPLETE_WITHIN_TIMEOUT_PERIOD:
        return _("Cannot complete within timeout period");

      case CIM_RETURNCODE_FAILED:
        return _("Failed");

      case CIM_RETURNCODE_INVALID_PARAMETER:
        return _("Invalid parameter");

      case CIM_RETURNCODE_IN_USE:
        return _("In use");

      case CIM_RETURNCODE_TRANSITION_STARTED:
        return _("Transition started");

      case CIM_RETURNCODE_INVALID_STATE_TRANSITION:
        return _("Invalid state transition");

      case CIM_RETURNCODE_TIMEOUT_PARAMETER_NOT_SUPPORTED:
        return _("Timeout parameter not supported");

      case CIM_RETURNCODE_BUSY:
        return _("Busy");

      case MSVM_RETURNCODE_FAILED:
        return _("Failed");

      case MSVM_RETURNCODE_ACCESS_DENIED:
        return _("Access denied");

      case MSVM_RETURNCODE_NOT_SUPPORTED:
        return _("Not supported");

      case MSVM_RETURNCODE_STATUS_IS_UNKNOWN:
        return _("Status is unknown");

      case MSVM_RETURNCODE_TIMEOUT:
        return _("Timeout");

      case MSVM_RETURNCODE_INVALID_PARAMETER:
        return _("Invalid parameter");

      case MSVM_RETURNCODE_SYSTEM_IS_IN_USE:
        return _("System is in use");

      case MSVM_RETURNCODE_INVALID_STATE_FOR_THIS_OPERATION:
        return _("Invalid state for this operation");

      case MSVM_RETURNCODE_INCORRECT_DATA_TYPE:
        return _("Incorrect data type");

      case MSVM_RETURNCODE_SYSTEM_IS_NOT_AVAILABLE:
        return _("System is not available");

      case MSVM_RETURNCODE_OUT_OF_MEMORY:
        return _("Out of memory");

      default:
        return _("Unknown return code");
    }
}

void rmSubstr(char *str, const char *toRemove)
{
    size_t length = strlen(toRemove);
    char *found,
         *next = strstr(str, toRemove);

    for (size_t bytesRemoved = 0; (found = next); bytesRemoved += length)
    {
        char *rest = found + length;
        next = strstr(rest, toRemove);
        memmove(found - bytesRemoved,
                rest,
                next ? next - rest: strlen(rest) + 1);
    }
}


int
hypervMsvmVirtualSwitchToNetwork(virConnectPtr conn,
                                 Msvm_VirtualSwitch *virtualSwitch, virNetworkPtr *network)
{
    unsigned char uuid[VIR_UUID_BUFLEN];
    char *rawUuid = NULL;

    if (network == NULL || *network != NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid argument"));
        return -1;
    }

    // Switch-SM-91b305f3-9a9e-408b-8b04-0ecff33aaba8-0
    // need to parse out the 'Switch-SM-' and '-0' so that its a proper UUID
    rawUuid = virtualSwitch->data->Name;
    rmSubstr(rawUuid, "Switch-SM-");
    char *finalUUID;

    finalUUID = (char *)calloc(37, sizeof(char));
    strncpy(finalUUID, rawUuid, 36);

    if (virUUIDParse(finalUUID, uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not parse UUID from string '%s'"),
                       finalUUID);
        free(finalUUID);
        return -1;
    }

    *network = virGetNetwork(conn, virtualSwitch->data->ElementName, uuid);

    if (*network == NULL) {
        free(finalUUID);
        return -1;
    }

    free(finalUUID);
    return 0;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * hypervInvokeMethod
 *   Function to invoke WSMAN request with simple, EPR or embedded parameters
 */

/* Create XML structure */
static int
hypervCreateXmlStruct(const char *methodName, const char *classURI,
                      WsXmlDocH *xmlDocRoot, WsXmlNodeH *xmlNodeMethod)
{
    virBuffer method_buff = VIR_BUFFER_INITIALIZER;
    char *methodNameInput = NULL;

    virBufferAsprintf(&method_buff, "%s_INPUT", methodName);
    methodNameInput = virBufferContentAndReset(&method_buff);

    if (methodNameInput == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Could not create Xml Doc"));
        goto cleanup;
    }

    *xmlDocRoot = ws_xml_create_doc(NULL, methodNameInput);
    if (*xmlDocRoot == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Could not create Xml Doc with given parameter xmlDocRoot"));
        goto cleanup;
    }

    *xmlNodeMethod = xml_parser_get_root(*xmlDocRoot);
    if (*xmlNodeMethod == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Could not get xmlDocRoot root node"));
        goto cleanup;
    }

    /* Add namespace to xmlNodeMethode */
    ws_xml_set_ns(*xmlNodeMethod, classURI, "p");

    VIR_FREE(methodNameInput);
    return 0;

 cleanup:

    virBufferFreeAndReset(&method_buff);
    VIR_FREE(methodNameInput);
    if (*xmlDocRoot != NULL) {
        ws_xml_destroy_doc(*xmlDocRoot);
        *xmlDocRoot = NULL;
    }

    return -1;
}


/* Look for the type of a given property class and specifies if it is an array */
static int
hypervGetPropType(const char *className, const char *propName, const char **propType, bool *isArray)
{
    int i, y;

    i = 0;
    while (cimClasses[i].name[0] != '\0') {
        if (STREQ(cimClasses[i].name, className)){
            y = 0;
            while (cimClasses[i].cimTypesPtr[y].name[0] != '\0') {
                if (STREQ(cimClasses[i].cimTypesPtr[y].name, propName)){
                    *propType = cimClasses[i].cimTypesPtr[y].type;
                    *isArray = cimClasses[i].cimTypesPtr[y].isArray;
                    return 0;
                }
                y++;
            }
            break;
        }
        i++;
    }

    return -1;
}


/* Adding an Simple param node to a parent node given in parameter */
static int
hypervAddSimpleParam(const char *paramName, const char* value,
                     const char *classURI, WsXmlNodeH *parentNode)
{
    int result = -1;
    WsXmlNodeH xmlNodeParam = NULL;

    xmlNodeParam = ws_xml_add_child(*parentNode, classURI, paramName, value);
    if (xmlNodeParam == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not create simple param"));
        goto cleanup;
    }

    result = 0;

 cleanup:
    return result;
}


/* Adding EPR param node to a parent node given in parameter */
static int
hypervAddEprParam(const char *paramName, virBufferPtr query, const char *root,
                  const char *classURI, WsXmlNodeH *parentNode, WsXmlDocH doc, hypervPrivate *priv)
{

    int result = -1;
    WsXmlNodeH xmlNodeParam = NULL;
    WsXmlNodeH xmlNodTemp = NULL;
    WsXmlNodeH xmlNodeAdr = NULL;
    WsXmlNodeH xmlNodeRef = NULL;
    xmlNodePtr xmlNodeAdrPtr = NULL;
    xmlNodePtr xmlNodeRefPtr = NULL;
    WsXmlDocH xmlDocResponse = NULL;
    xmlDocPtr docPtr = (xmlDocPtr) doc->parserDoc;
    WsXmlNsH ns = NULL;
    client_opt_t *options = NULL;
    filter_t *filter = NULL;
    char *enumContext = NULL;
    char *query_string = NULL;

    /* Request options and filter */
    options = wsmc_options_init();

    if (options == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not initialize options"));
        goto cleanup;
    }

    wsmc_set_action_option(options, FLAG_ENUMERATION_ENUM_EPR);

    query_string = virBufferContentAndReset(query);
    filter = filter_create_simple(WSM_WQL_FILTER_DIALECT, query_string);
    if (filter == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not create filter"));
        goto cleanup;
    }

    /* Invoke enumerate action*/
    xmlDocResponse = wsmc_action_enumerate(priv->client,root, options, filter);

    /* Check return value */
    if (hyperyVerifyResponse(priv->client, xmlDocResponse, "enumeration") < 0) {
        goto cleanup;
    }

    /* Get enumerate conext*/
    enumContext = wsmc_get_enum_context(xmlDocResponse);

    ws_xml_destroy_doc(xmlDocResponse);


    /* Invoke pull action*/
    xmlDocResponse = wsmc_action_pull(priv->client, classURI, options, filter, enumContext);

    /* Check return value */
    if (hyperyVerifyResponse(priv->client, xmlDocResponse, "pull") < 0) {
        goto cleanup;
    }

    /* Extract EPR nodes childs */
    xmlNodTemp = ws_xml_get_soap_body(xmlDocResponse);
    if (xmlNodTemp == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not lookup SOAP body"));
        goto cleanup;
    }

    xmlNodTemp = ws_xml_get_child(xmlNodTemp, 0, XML_NS_ENUMERATION, WSENUM_PULL_RESP);
    if (xmlNodTemp == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not lookup pull response"));
        goto cleanup;
    }

    xmlNodTemp = ws_xml_get_child(xmlNodTemp, 0, XML_NS_ENUMERATION, WSENUM_ITEMS);
    if (xmlNodTemp == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not lookup pull response items"));
        goto cleanup;
    }

    xmlNodTemp = ws_xml_get_child(xmlNodTemp, 0, XML_NS_ADDRESSING, WSA_EPR);
    if (xmlNodTemp == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not lookup pull response item EPR"));
        goto cleanup;
    }

    xmlNodeAdr = ws_xml_get_child(xmlNodTemp, 0, XML_NS_ADDRESSING, WSA_ADDRESS);
    if (xmlNodeAdr == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not lookup pull response item ADDRESS"));
        goto cleanup;
    }
    xmlNodeAdrPtr = xmlDocCopyNode((xmlNodePtr) xmlNodeAdr, docPtr, 1);
    if (xmlNodeAdrPtr == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not copy item ADDRESS"));
        goto cleanup;
    }

    xmlNodeRef = ws_xml_get_child(xmlNodTemp, 0, XML_NS_ADDRESSING, WSA_REFERENCE_PARAMETERS);
    if (xmlNodeRef == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not lookup pull response item REFERENCE PARAMETERS"));
        goto cleanup;
    }
    xmlNodeRefPtr = xmlDocCopyNode((xmlNodePtr) xmlNodeRef, docPtr, 1);
    if (xmlNodeRefPtr == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not copy item REFERENCE PARAMETERS"));
        goto cleanup;
    }

    /* Build XmlDoc with adding previous EPR nodes childs */
    xmlNodeParam = ws_xml_add_child(*parentNode, classURI, paramName, NULL);
    if (xmlNodeParam == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not add child node to xmlNodeParam"));
        goto cleanup;
    }

/*
  The folowing line has been commented because of a memory corruption issue reported in the openwsman library
  [ issue #43 - xml_parser_ns_add: alloc item size, not pointer size ]
  xmlNodeSetLang((xmlNodePtr) xmlNodeParam, BAD_CAST "en-US");
*/
    ns = ws_xml_ns_add(xmlNodeParam, "http://schemas.xmlsoap.org/ws/2004/08/addressing", "a");
    if (ns == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not set namespace adressing to xmlNodeParam"));
        goto cleanup;
    }

    ns = NULL;
    ns = ws_xml_ns_add(xmlNodeParam, "http://schemas.dmtf.org/wbem/wsman/1/wsman.xsd", "w");
    if (ns == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not set namespace wsman to xmlNodeParam"));
        goto cleanup;
    }

    if (xmlAddChild((xmlNodePtr) *parentNode,(xmlNodePtr) xmlNodeParam) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not add child to xml parent node"));
        goto cleanup;
    }

    if (xmlAddChild((xmlNodePtr) xmlNodeParam, xmlNodeAdrPtr) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not add child to xml parent node"));
        goto cleanup;
    }

    if (xmlAddChild((xmlNodePtr) xmlNodeParam, xmlNodeRefPtr) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not add child to xml parent node"));
        goto cleanup;
    }

    result = 0;

 cleanup:
    if (options != NULL) {
        wsmc_options_destroy(options);
    }
    if (filter != NULL) {
        filter_destroy(filter);
    }
    ws_xml_destroy_doc(xmlDocResponse);
    VIR_FREE(enumContext);
    VIR_FREE(query_string);

    return result;
}


/* Adding an Embedded Instance node to a parent node given in parameter */
static int
hypervAddEmbeddedParam(properties_t *prop_t, int nbProps, const char *paramName,
                       const char *instanceName, const char *classURI, WsXmlNodeH *parentNode)
{

    int result = -1;
    WsXmlNodeH xmlNodeInstance = NULL;
    WsXmlNodeH xmlNodeProperty = NULL;
    WsXmlNodeH xmlNodeParam = NULL;
    WsXmlNodeH xmlNodeArray = NULL;
    WsXmlDocH xmlDocTemp = NULL;
    WsXmlDocH xmlDocCdata = NULL;
    xmlBufferPtr xmlBufferNode = NULL;
    const xmlChar *xmlCharCdataContent = NULL;
    xmlNodePtr xmlNodeCdata = NULL;
    const char *type = NULL;
    bool isArray = false;
    int len = 0;
    int	i = 0;

    /* Add child to given parent node*/
    xmlNodeParam = ws_xml_add_child(*parentNode, classURI, paramName, NULL);
    if (xmlNodeParam == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not add child node to xmlNodeParam"));
        goto cleanup;
    }

    /* Create temp Xml doc */
    /* INSTANCE node */
    xmlDocTemp = ws_xml_create_doc(NULL, "INSTANCE");
    if (xmlDocTemp == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Could not create temporary Xml doc"));
        goto cleanup;
    }

    xmlNodeInstance = xml_parser_get_root(xmlDocTemp);
    if (xmlNodeInstance == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Could not get root of temporary Xml doc"));
        goto cleanup;
    }

    /* Add CLASSNAME node to INSTANCE node */
    if (ws_xml_add_node_attr(xmlNodeInstance, NULL, "CLASSNAME", instanceName) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Could not add attribute to node "));
        goto cleanup;
    }

    /* Property nodes */
    while (i < nbProps) {

        if (prop_t[i].name == NULL && prop_t[i].val == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s",
                           _("Could not get properties from array"));
            goto cleanup;
        }

        if (hypervGetPropType(instanceName, prop_t[i].name, &type, &isArray) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s",
                           _("Could not get properties from array"));
            goto cleanup;
        }

        xmlNodeProperty = ws_xml_add_child(xmlNodeInstance, NULL,
                                           isArray ? "PROPERTY.ARRAY" : "PROPERTY", NULL);
        if (xmlNodeProperty == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s",
                           _("Could not add child to node"));
            goto cleanup;
        }

        if (ws_xml_add_node_attr(xmlNodeProperty, NULL, "NAME", prop_t[i].name) == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s",
                           _("Could not add attribute to node"));
            goto cleanup;
        }

        if (ws_xml_add_node_attr(xmlNodeProperty, NULL, "TYPE", type) == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s",
                           _("Could not add attribute to node"));
            goto cleanup;
        }

        /* Add the node VALUE.ARRAY if the attribute is an array */
        if (isArray) {
            xmlNodeArray = ws_xml_add_child(xmlNodeProperty, NULL, "VALUE.ARRAY", NULL);
            if (xmlNodeArray == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               "%s",
                               _("Could not add child to node"));
                goto cleanup;
            }
        }

        if (ws_xml_add_child(isArray ? xmlNodeArray : xmlNodeProperty, NULL, "VALUE", prop_t[i].val) == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s",
                           _("Could not add child to node"));
            goto cleanup;
        }

        xmlNodeArray = NULL;
        xmlNodeProperty = NULL;
        i++;
    }

    /* Create CDATA node */
    xmlBufferNode = xmlBufferCreate();
    if (xmlNodeDump(xmlBufferNode, (xmlDocPtr) xmlDocTemp->parserDoc, (xmlNodePtr) xmlNodeInstance, 0, 0) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Could not get root of temporary Xml doc"));
        goto cleanup;
    }

    len = xmlBufferLength(xmlBufferNode);
    xmlCharCdataContent = xmlBufferContent(xmlBufferNode);
    xmlNodeCdata = xmlNewCDataBlock((xmlDocPtr) xmlDocCdata, xmlCharCdataContent, len);
    if (xmlNodeCdata == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Could not get root of temporary Xml doc"));
        goto cleanup;
    }
    /* Add CDATA node child to the root node of the main doc given */
    if (xmlAddChild((xmlNodePtr) xmlNodeParam, xmlNodeCdata) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Could not get root of temporary Xml doc"));
        goto cleanup;
    }

    result = 0;

 cleanup:
    ws_xml_destroy_doc(xmlDocCdata);
    ws_xml_destroy_doc(xmlDocTemp);
    if (xmlBufferNode != NULL)
        xmlBufferFree(xmlBufferNode);

    return result;
}

/* Call wsmc_action_invoke() function of OpenWsman API with XML tree given in parameters*/
static int
hypervInvokeMethodXml(hypervPrivate *priv, WsXmlDocH xmlDocRoot,
                      const char *methodName, const char *ressourceURI, const char *selector)
{
    int result = -1;
    int returnCode;
    char *instanceID = NULL;
    char *xpath_expr_string = NULL;
    char *returnValue = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    virBuffer xpath_expr_buff = VIR_BUFFER_INITIALIZER;
    client_opt_t *options = NULL;
    WsXmlDocH response = NULL;
    Msvm_ConcreteJob *concreteJob = NULL;
    Msvm_ConcreteJob_2012 *concreteJob2012 = NULL;
    int concreteJobState = -1;
    bool completed = false;

    options = wsmc_options_init();

    if (options == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not initialize options"));
        goto cleanup;
    }

    wsmc_add_selectors_from_str(options, selector);

    /* Invoke action */
    response = wsmc_action_invoke(priv->client, ressourceURI, options, methodName, xmlDocRoot);

    virBufferAsprintf(&xpath_expr_buff, "/s:Envelope/s:Body/p:%s_OUTPUT/p:ReturnValue", methodName);
    xpath_expr_string = virBufferContentAndReset(&xpath_expr_buff);

    if (xpath_expr_string == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for %s invocation"),
                       "ReturnValue", "RequestStateChange");
        goto cleanup;
    }

    /* Check return value */
    returnValue = ws_xml_get_xpath_value(response, xpath_expr_string);

    VIR_FREE(xpath_expr_string);
    xpath_expr_string = NULL;

    if (returnValue == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for %s invocation"),
                       "ReturnValue", "RequestStateChange");
        goto cleanup;
    }

    if (virStrToLong_i(returnValue, NULL, 10, &returnCode) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not parse return code from '%s'"), returnValue);
        goto cleanup;
    }

    if (returnCode == CIM_RETURNCODE_TRANSITION_STARTED) {
        virBufferAsprintf(&xpath_expr_buff, "/s:Envelope/s:Body/p:%s_OUTPUT/p:Job/a:ReferenceParameters/w:SelectorSet/w:Selector[@Name='InstanceID']", methodName);
        xpath_expr_string = virBufferContentAndReset(&xpath_expr_buff);
        if (xpath_expr_string == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not lookup %s for %s invocation"),
                           "InstanceID", "RequestStateChange");
            goto cleanup;
        }

        /* Get concrete job object */
        instanceID = ws_xml_get_xpath_value(response, xpath_expr_string);
        if (instanceID == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not lookup %s for %s invocation"),
                           "InstanceID", "RequestStateChange");
            goto cleanup;
        }

        /* FIXME: Poll every 100ms until the job completes or fails.
         *        There seems to be no other way than polling. */
        while (!completed) {
            if (strcmp(priv->hypervVersion, HYPERV_VERSION_2008) == 0) {
                virBufferAddLit(&query, MSVM_CONCRETEJOB_WQL_SELECT);
                virBufferAsprintf(&query, "where InstanceID = \"%s\"", instanceID);

                if (hypervGetMsvmConcreteJobList(priv, &query, &concreteJob) < 0) {
                    goto cleanup;
                }

                concreteJobState = concreteJob->data->JobState;
            } else if (strcmp(priv->hypervVersion, HYPERV_VERSION_2012) == 0) {
                virBufferAddLit(&query, MSVM_CONCRETEJOB_2012_WQL_SELECT);
                virBufferAsprintf(&query, "where InstanceID = \"%s\"", instanceID);

                if (hypervGetMsvmConcreteJob2012List(priv, &query, &concreteJob2012) < 0) {
                    goto cleanup;
                }

                concreteJobState = concreteJob2012->data->JobState;
            }

            if (concreteJob == NULL && concreteJob2012 == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Could not lookup %s for %s invocation"),
                               "Msvm_ConcreteJob", "RequestStateChange");
                goto cleanup;
            }

            switch (concreteJobState) {
                case MSVM_CONCRETEJOB_JOBSTATE_NEW:
                case MSVM_CONCRETEJOB_JOBSTATE_STARTING:
                case MSVM_CONCRETEJOB_JOBSTATE_RUNNING:
                case MSVM_CONCRETEJOB_JOBSTATE_SHUTTING_DOWN:
                    hypervFreeObject(priv, (hypervObject *) concreteJob);
                    hypervFreeObject(priv, (hypervObject *) concreteJob2012);
                    concreteJob = NULL;
                    concreteJob2012 = NULL;
                    usleep(100 * 1000);  /* Wait 100 ms */
                    continue;

                case MSVM_CONCRETEJOB_JOBSTATE_COMPLETED:
                    completed = true;
                    break;

                case MSVM_CONCRETEJOB_JOBSTATE_TERMINATED:
                case MSVM_CONCRETEJOB_JOBSTATE_KILLED:
                case MSVM_CONCRETEJOB_JOBSTATE_EXCEPTION:
                case MSVM_CONCRETEJOB_JOBSTATE_SERVICE:
                    goto cleanup;

                default:
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("Concrete job for %s invocation is in unknown state"),
                                   "RequestStateChange");
                    goto cleanup;
            }
        }
    }
    else if (returnCode != CIM_RETURNCODE_COMPLETED_WITH_NO_ERROR) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invocation of %s returned an error: %s (%d)"),
                       "RequestStateChange", hypervReturnCodeToString(returnCode),
                       returnCode);
        goto cleanup;
    }

    result = 0;

 cleanup:
    if (options != NULL)
        wsmc_options_destroy(options);
    if (response != NULL)
        ws_xml_destroy_doc(response);
    VIR_FREE(returnValue);
    VIR_FREE(instanceID);
    VIR_FREE(xpath_expr_string);
    hypervFreeObject(priv, (hypervObject *) concreteJob);
    hypervFreeObject(priv, (hypervObject *) concreteJob2012);
    virBufferFreeAndReset(&query);
    virBufferFreeAndReset(&xpath_expr_buff);

    return result;
}


/* Calls the invoke method by passing provided parameters as an XML tree */
int
hypervInvokeMethod(hypervPrivate *priv, invokeXmlParam *param_t, int nbParameters,
                   const char* methodName, const char* providerURI, const char *selector)
{
    int result = -1;
    WsXmlDocH doc = NULL;
    WsXmlNodeH methodNode = NULL;
    simpleParam *simple;
    eprParam *epr;
    embeddedParam *embedded;
    int i =0;

    if (hypervCreateXmlStruct(methodName,providerURI,&doc,&methodNode) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Could not create xml base structure"));
        goto cleanup;
    }

    /* Process parameters among the three allowed types */
    while ( i < nbParameters) {
        switch (param_t[i].type) {
            case SIMPLE_PARAM:
                simple = (simpleParam *) param_t[i].param;
                if (hypervAddSimpleParam(param_t[i].name,simple->value, providerURI, &methodNode) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s",
                                   _("Could not add embedded instance param to xml base structure"));
                    goto cleanup;
                }
                break;
            case EPR_PARAM:
                epr = (eprParam *) param_t[i].param;
                if (hypervAddEprParam(param_t[i].name, epr->query, epr->wmiProviderURI, providerURI, &methodNode, doc, priv) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s",
                                   _("Could not add EPR param to xml base structure"));
                    goto cleanup;
                }
                break;
            case EMBEDDED_PARAM:
                embedded = (embeddedParam *) param_t[i].param;
                if (hypervAddEmbeddedParam(embedded->prop_t, embedded->nbProps, param_t[i].name, embedded->instanceName, providerURI, &methodNode) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s",
                                   _("Could not add embedded instance param to xml base structure"));
                    goto cleanup;
                }
                break;
        }
        i++;
    }

    /* Call the invoke method */
    if (hypervInvokeMethodXml(priv, doc, methodName, providerURI, selector) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Error during invocation action"));
        goto cleanup;
    }

    result = 0;

 cleanup:
    if (doc != NULL)
        ws_xml_destroy_doc(doc);

    return result;
}

/* Call wsmc_action_invoke() function of OpenWsman API with XML tree given in parameters*/
static thumbnailImage *
hypervGetVirtualSystemThumbnailImageXml(hypervPrivate *priv, WsXmlDocH xmlDocRoot,
                      const char *methodName, const char *ressourceURI, const char *selector)
{
    int returnCode;
    int childCount;
    int i;
    char *instanceID = NULL;
    char *xpath_expr_string = NULL;
    char *returnValue = NULL;
    char *nameSpace = NULL;
    char *imageDataText = NULL;
    char *imageDataBuffer = NULL;
    thumbnailImage *result = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    virBuffer xpath_expr_buff = VIR_BUFFER_INITIALIZER;
    client_opt_t *options = NULL;
    WsXmlNodeH envelope = NULL;
    WsXmlNodeH body = NULL;
    WsXmlNodeH thumbnail = NULL;
    WsXmlNodeH imageData = NULL;
    WsXmlDocH response = NULL;

    options = wsmc_options_init();

    if (options == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not initialize options"));
        goto cleanup;
    }

    wsmc_add_selectors_from_str(options, selector);

    /* Invoke action */
    response = wsmc_action_invoke(priv->client, ressourceURI, options, methodName, xmlDocRoot);

    virBufferAsprintf(&xpath_expr_buff, "/s:Envelope/s:Body/p:%s_OUTPUT/p:ReturnValue", methodName);
    xpath_expr_string = virBufferContentAndReset(&xpath_expr_buff);

    if (xpath_expr_string == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for %s invocation"),
                       "ReturnValue", "RequestStateChange");
        goto cleanup;
    }

    /* Check return value */
    returnValue = ws_xml_get_xpath_value(response, xpath_expr_string);

    VIR_FREE(xpath_expr_string);
    xpath_expr_string = NULL;

    if (returnValue == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for %s invocation"),
                       "ReturnValue", "RequestStateChange");
        goto cleanup;
    }

    if (virStrToLong_i(returnValue, NULL, 10, &returnCode) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not parse return code from '%s'"), returnValue);
        goto cleanup;
    }

    // @todo check returnValue and act appropriately

    // go to the node that has the data
    envelope = ws_xml_get_soap_envelope(response);
    body = ws_xml_get_child(envelope, 1, NULL, NULL);
    thumbnail = ws_xml_get_child(body, 0, NULL, NULL);

    if (envelope == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Blah!"));
        goto cleanup;
    } else {
        childCount = ws_xml_get_child_count(thumbnail);
        nameSpace = ws_xml_get_node_name_ns(thumbnail);

        // pjr - this is debug - remove!!
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Child count: %d, namespace: %s"), childCount, nameSpace);
    }

    // iterate over the data and collect it together
    imageDataBuffer = (char *)calloc(childCount+1, sizeof(char));

    for (i=0; i < childCount; i++) {
        imageData = ws_xml_get_child(thumbnail, i, NULL, NULL);
        imageDataText = ws_xml_get_node_text(imageData);
        imageDataBuffer[i] = (char)atoi(imageDataText);
    }

    // return the data
    if (VIR_ALLOC(result) < 0)
        goto cleanup;

    result->data = imageDataBuffer;
    result->length = childCount;

    cleanup:
    if (options != NULL)
        wsmc_options_destroy(options);
    if (response != NULL)
        ws_xml_destroy_doc(response);
    VIR_FREE(returnValue);
    VIR_FREE(instanceID);
    VIR_FREE(xpath_expr_string);
    virBufferFreeAndReset(&query);
    virBufferFreeAndReset(&xpath_expr_buff);

    return result;
}

/* Calls the get thumbnails method by passing provided parameters as an XML tree */
thumbnailImage *
hypervGetVirtualSystemThumbnailImage(hypervPrivate *priv, invokeXmlParam *param_t, int nbParameters,
                                     const char* providerURI, const char *selector) {
    const char* methodName = "GetVirtualSystemThumbnailImage";
    thumbnailImage *result = NULL;
    WsXmlDocH doc = NULL;
    WsXmlNodeH methodNode = NULL;
    simpleParam *simple;
    eprParam *epr;
    embeddedParam *embedded;
    int i =0;

    if (hypervCreateXmlStruct(methodName,providerURI,&doc,&methodNode) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Could not create xml base structure"));
        goto cleanup;
    }

    /* Process parameters among the three allowed types */
    while ( i < nbParameters) {
        switch (param_t[i].type) {
            case SIMPLE_PARAM:
                simple = (simpleParam *) param_t[i].param;
                if (hypervAddSimpleParam(param_t[i].name,simple->value, providerURI, &methodNode) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s",
                                   _("Could not add embedded instance param to xml base structure"));
                    goto cleanup;
                }
                break;
            case EPR_PARAM:
                epr = (eprParam *) param_t[i].param;
                if (hypervAddEprParam(param_t[i].name, epr->query, epr->wmiProviderURI, providerURI, &methodNode, doc, priv) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s",
                                   _("Could not add EPR param to xml base structure"));
                    goto cleanup;
                }
                break;
            case EMBEDDED_PARAM:
                embedded = (embeddedParam *) param_t[i].param;
                if (hypervAddEmbeddedParam(embedded->prop_t, embedded->nbProps, param_t[i].name, embedded->instanceName, providerURI, &methodNode) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s",
                                   _("Could not add embedded instance param to xml base structure"));
                    goto cleanup;
                }
                break;
        }
        i++;
    }

    /* Call the invoke method */
    result = hypervGetVirtualSystemThumbnailImageXml(priv, doc, methodName, providerURI, selector);

    if (result == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Error during invocation action"));
    }

    cleanup:
    if (doc != NULL)
        ws_xml_destroy_doc(doc);

    return result;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Msvm_ComputerSystem
 */

int
hypervInvokeMsvmComputerSystemRequestStateChange(virDomainPtr domain,
                                                 int requestedState)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    WsXmlDocH response = NULL;
    client_opt_t *options = NULL;
    char *selector = NULL;
    char *properties = NULL;
    char *returnValue = NULL;
    int returnCode;
    char *instanceID = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ConcreteJob *concreteJob = NULL;
    bool completed = false;

    virUUIDFormat(domain->uuid, uuid_string);

    if (virAsprintf(&selector, "Name=%s&CreationClassName=Msvm_ComputerSystem",
                    uuid_string) < 0 ||
        virAsprintf(&properties, "RequestedState=%d", requestedState) < 0)
        goto cleanup;

    options = wsmc_options_init();

    if (options == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not initialize options"));
        goto cleanup;
    }

    wsmc_add_selectors_from_str(options, selector);
    wsmc_add_prop_from_str(options, properties);

    /* Invoke method */
    response = wsmc_action_invoke(priv->client, MSVM_COMPUTERSYSTEM_RESOURCE_URI,
                                  options, "RequestStateChange", NULL);

    if (hyperyVerifyResponse(priv->client, response, "invocation") < 0)
        goto cleanup;

    /* Check return value */
    returnValue = ws_xml_get_xpath_value(response, (char *)"/s:Envelope/s:Body/p:RequestStateChange_OUTPUT/p:ReturnValue");

    if (returnValue == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for %s invocation"),
                       "ReturnValue", "RequestStateChange");
        goto cleanup;
    }

    if (virStrToLong_i(returnValue, NULL, 10, &returnCode) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not parse return code from '%s'"), returnValue);
        goto cleanup;
    }

    if (returnCode == CIM_RETURNCODE_TRANSITION_STARTED) {
        /* Get concrete job object */
        instanceID = ws_xml_get_xpath_value(response, (char *)"/s:Envelope/s:Body/p:RequestStateChange_OUTPUT/p:Job/a:ReferenceParameters/w:SelectorSet/w:Selector[@Name='InstanceID']");

        if (instanceID == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not lookup %s for %s invocation"),
                           "InstanceID", "RequestStateChange");
            goto cleanup;
        }

        /* FIXME: Poll every 100ms until the job completes or fails. There
         *        seems to be no other way than polling. */
        while (!completed) {
            virBufferAddLit(&query, MSVM_CONCRETEJOB_WQL_SELECT);
            virBufferAsprintf(&query, "where InstanceID = \"%s\"", instanceID);

            if (hypervGetMsvmConcreteJobList(priv, &query, &concreteJob) < 0)
                goto cleanup;

            if (concreteJob == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Could not lookup %s for %s invocation"),
                               "Msvm_ConcreteJob", "RequestStateChange");
                goto cleanup;
            }

            switch (concreteJob->data->JobState) {
              case MSVM_CONCRETEJOB_JOBSTATE_NEW:
              case MSVM_CONCRETEJOB_JOBSTATE_STARTING:
              case MSVM_CONCRETEJOB_JOBSTATE_RUNNING:
              case MSVM_CONCRETEJOB_JOBSTATE_SHUTTING_DOWN:
                hypervFreeObject(priv, (hypervObject *)concreteJob);
                concreteJob = NULL;

                usleep(100 * 1000);
                continue;

              case MSVM_CONCRETEJOB_JOBSTATE_COMPLETED:
                completed = true;
                break;

              case MSVM_CONCRETEJOB_JOBSTATE_TERMINATED:
              case MSVM_CONCRETEJOB_JOBSTATE_KILLED:
              case MSVM_CONCRETEJOB_JOBSTATE_EXCEPTION:
              case MSVM_CONCRETEJOB_JOBSTATE_SERVICE:
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Concrete job for %s invocation is in error state"),
                               "RequestStateChange");
                goto cleanup;

              default:
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Concrete job for %s invocation is in unknown state"),
                               "RequestStateChange");
                goto cleanup;
            }
        }
    } else if (returnCode != CIM_RETURNCODE_COMPLETED_WITH_NO_ERROR) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invocation of %s returned an error: %s (%d)"),
                       "RequestStateChange", hypervReturnCodeToString(returnCode),
                       returnCode);
        goto cleanup;
    }

    result = 0;

 cleanup:
    if (options != NULL)
        wsmc_options_destroy(options);

    ws_xml_destroy_doc(response);
    VIR_FREE(selector);
    VIR_FREE(properties);
    VIR_FREE(returnValue);
    VIR_FREE(instanceID);
    hypervFreeObject(priv, (hypervObject *)concreteJob);

    return result;
}

int
hypervMsvmComputerSystemEnabledStateToDomainState
  (Msvm_ComputerSystem *computerSystem)
{
    switch (computerSystem->data->EnabledState) {
      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_UNKNOWN:
        return VIR_DOMAIN_NOSTATE;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_ENABLED:
        return VIR_DOMAIN_RUNNING;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_DISABLED:
        return VIR_DOMAIN_SHUTOFF;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_PAUSED:
        return VIR_DOMAIN_PAUSED;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SUSPENDED: /* managed save */
        return VIR_DOMAIN_SHUTOFF;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_STARTING:
      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SNAPSHOTTING:
      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SAVING:
        return VIR_DOMAIN_RUNNING;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_STOPPING:
        return VIR_DOMAIN_SHUTDOWN;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_PAUSING:
      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_RESUMING:
        return VIR_DOMAIN_RUNNING;

      default:
        return VIR_DOMAIN_NOSTATE;
    }
}

bool
hypervIsMsvmComputerSystemActive(Msvm_ComputerSystem *computerSystem,
                                 bool *in_transition)
{
    if (in_transition != NULL)
        *in_transition = false;

    switch (computerSystem->data->EnabledState) {
      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_UNKNOWN:
        return false;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_ENABLED:
        return true;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_DISABLED:
        return false;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_PAUSED:
        return true;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SUSPENDED: /* managed save */
        return false;

      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_STARTING:
      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SNAPSHOTTING:
      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SAVING:
      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_STOPPING:
      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_PAUSING:
      case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_RESUMING:
        if (in_transition != NULL)
            *in_transition = true;

        return true;

      default:
        return false;
    }
}

int
hypervMsvmComputerSystemToDomain(virConnectPtr conn,
                                 Msvm_ComputerSystem *computerSystem,
                                 virDomainPtr *domain)
{
    unsigned char uuid[VIR_UUID_BUFLEN];

    if (domain == NULL || *domain != NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid argument"));
        return -1;
    }

    if (virUUIDParse(computerSystem->data->Name, uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not parse UUID from string '%s'"),
                       computerSystem->data->Name);
        return -1;
    }

    *domain = virGetDomain(conn, computerSystem->data->ElementName, uuid);

    if (*domain == NULL)
        return -1;

    if (hypervIsMsvmComputerSystemActive(computerSystem, NULL)) {
        (*domain)->id = computerSystem->data->ProcessID;
    } else {
        (*domain)->id = -1;
    }

    return 0;
}

int
hypervMsvmComputerSystemFromDomain(virDomainPtr domain,
                                   Msvm_ComputerSystem **computerSystem)
{
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;

    if (computerSystem == NULL || *computerSystem != NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid argument"));
        return -1;
    }

    virUUIDFormat(domain->uuid, uuid_string);

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and Name = \"%s\"", uuid_string);

    if (hypervGetMsvmComputerSystemList(priv, &query, computerSystem) < 0)
        return -1;

    if (*computerSystem == NULL) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("No domain with UUID %s"), uuid_string);
        return -1;
    }

    return 0;
}

int
hypervMsvmVirtualHardDiskSettingFromDomain(virDomainPtr domain,
                                            Msvm_VirtualHardDiskSettingData **virtualHardDisk)
{
    //hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    //virBuffer query = VIR_BUFFER_INITIALIZER;

    if (virtualHardDisk == NULL || *virtualHardDisk != NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid argument"));
        return -1;
    }

    virUUIDFormat(domain->uuid, uuid_string);

    return 0;
}



#include "hyperv_wmi.generated.c"
