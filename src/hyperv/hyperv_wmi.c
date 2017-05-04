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
#include <wsman-soap.h>

#include "internal.h"
#include "virerror.h"
#include "datatypes.h"
#include "viralloc.h"
#include "viruuid.h"
#include "virbuffer.h"
#include "virlog.h"
#include "hyperv_private.h"
#include "hyperv_wmi.h"
#include "virstring.h"
#include "hyperv_wmi_cimtypes.generated.h"

VIR_LOG_INIT("hyperv.hyperv_wmi")

#define WS_SERIALIZER_FREE_MEM_WORKS 0


#define VIR_FROM_THIS VIR_FROM_HYPERV


void
hypervDebugResponseXml(WsXmlDocH response)
{
#ifdef ENABLE_DEBUG
    char *buf = NULL;
    int len;

    ws_xml_dump_memory_enc(response, &buf, &len, "UTF-8");

    if (buf && len > 0)
        VIR_DEBUG("%s", buf);

    ws_xml_free_memory(buf);
#endif
}

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
 * Method
 */

/* Methods to deal with building and invoking WMI methods over SOAP */

/* create xml struct */
int
hypervCreateXmlStruct(const char *methodName, const char *classURI,
        WsXmlDocH *xmlDocRoot, WsXmlNodeH *xmlNodeMethod)
{
    virBuffer method_buf = VIR_BUFFER_INITIALIZER;
    char *methodNameInput = NULL;

    virBufferAsprintf(&method_buf, "%s_INPUT", methodName);
    methodNameInput = virBufferContentAndReset(&method_buf);

    if (!methodNameInput) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not create XML document"));
        goto cleanup;
    }

    *xmlDocRoot = ws_xml_create_doc(NULL, methodNameInput);
    if (*xmlDocRoot == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not create XML doc with given parameter xmlDocRoot"));
        goto cleanup;
    }

    *xmlNodeMethod = xml_parser_get_root(*xmlDocRoot);
    if (*xmlNodeMethod == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not get xmlDocRoot root node"));
        goto cleanup;
    }

    /* Add namespace to xmlNodeMethod */
    ws_xml_set_ns(*xmlNodeMethod, classURI, "p");

    VIR_FREE(methodNameInput);
    return 0;

cleanup:
    virBufferFreeAndReset(&method_buf);
    VIR_FREE(methodNameInput);
    if (*xmlDocRoot != NULL) {
        ws_xml_destroy_doc(*xmlDocRoot);
        *xmlDocRoot = NULL;
    }
    return -1;
}

/* determines whether property class type is array and sets output values
 * accordingly.
 */
static int
hypervGetPropType(const char *className, const char *propName,
        const char **propType, bool *isArray)
{
    int i, y;

    i = 0;
    while (cimClasses[i].name[0] != '\0') {
        if (STREQ(cimClasses[i].name, className)) {
            y = 0;
            while (cimClasses[i].cimTypesPtr[y].name[0] != '\0') {
                if (STREQ(cimClasses[i].cimTypesPtr[y].name, propName)) {
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

/* Add a SIMPLE type param node to the parent node passed in */
int
hypervAddSimpleParam(const char *paramName, const char *value,
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

/* Add an EPR param to the parent node passed in */
int
hypervAddEprParam(const char *paramName, virBufferPtr query, const char *root,
        const char *classURI, WsXmlNodeH *parentNode, WsXmlDocH doc,
        hypervPrivate *priv)
{
    int result = -1;
    WsXmlNodeH xmlNodeParam = NULL,
               xmlNodeTemp = NULL,
               xmlNodeAddr = NULL,
               xmlNodeRef = NULL;
    xmlNodePtr xmlNodeAddrPtr = NULL,
               xmlNodeRefPtr = NULL;
    WsXmlDocH xmlDocResponse = NULL;
    xmlDocPtr docPtr = (xmlDocPtr) doc->parserDoc;
    WsXmlNsH ns = NULL;
    client_opt_t *options = NULL;
    filter_t *filter = NULL;
    char *enumContext = NULL;
    char *query_string = NULL;

    /* get options */
    options = wsmc_options_init();

    if (!options) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not init options"));
        goto cleanup;
    }

    wsmc_set_action_option(options, FLAG_ENUMERATION_ENUM_EPR);

    /* Get query and create filter based on it */
    query_string = virBufferContentAndReset(query);
    filter = filter_create_simple(WSM_WQL_FILTER_DIALECT, query_string);
    if (!filter) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not create filter"));
        goto cleanup;
    }

    /* Enumerate based on the filter from the query */
    xmlDocResponse = wsmc_action_enumerate(priv->client, root, options, filter);

    if (hyperyVerifyResponse(priv->client, xmlDocResponse, "enumeration") < 0)
        goto cleanup;

    /* Get context from response */
    enumContext = wsmc_get_enum_context(xmlDocResponse);
    ws_xml_destroy_doc(xmlDocResponse);

    /* Pull using filter and enum context */
    xmlDocResponse = wsmc_action_pull(priv->client, classURI, options, filter,
            enumContext);

    if (hyperyVerifyResponse(priv->client, xmlDocResponse, "pull") < 0)
        goto cleanup;

    /* drill down and extract EPR node children */
    if (!(xmlNodeTemp = ws_xml_get_soap_body(xmlDocResponse))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get SOAP body"));
        goto cleanup;
    }

    if (!(xmlNodeTemp = ws_xml_get_child(xmlNodeTemp, 0, XML_NS_ENUMERATION,
            WSENUM_PULL_RESP))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get response"));
        goto cleanup;
    }

    if (!(xmlNodeTemp = ws_xml_get_child(xmlNodeTemp, 0, XML_NS_ENUMERATION, WSENUM_ITEMS))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get response items"));
        goto cleanup;
    }

    if (!(xmlNodeTemp = ws_xml_get_child(xmlNodeTemp, 0, XML_NS_ADDRESSING, WSA_EPR))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get EPR items"));
        goto cleanup;
    }

    if (!(xmlNodeAddr = ws_xml_get_child(xmlNodeTemp, 0, XML_NS_ADDRESSING,
                    WSA_ADDRESS))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get EPR address"));
        goto cleanup;
    }

    if (!(xmlNodeAddrPtr = xmlDocCopyNode((xmlNodePtr) xmlNodeAddr, docPtr, 1))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not copy EPR address"));
        goto cleanup;
    }

    if (!(xmlNodeRef = ws_xml_get_child(xmlNodeTemp, 0, XML_NS_ADDRESSING,
            WSA_REFERENCE_PARAMETERS))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not lookup EPR item reference parameters"));
        goto cleanup;
    }

    if (!(xmlNodeRefPtr = xmlDocCopyNode((xmlNodePtr) xmlNodeRef, docPtr, 1))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not copy EPR item reference parameters"));
        goto cleanup;
    }

    /* we did it, now build a new xml doc with the EPR node children */
    if (!(xmlNodeParam = ws_xml_add_child(*parentNode, classURI, paramName,
                    NULL))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not add child node to xmlNodeParam"));
        goto cleanup;
    }

    if (!(ns = ws_xml_ns_add(xmlNodeParam,
                    "http://schemas.xmlsoap.org/ws/2004/08/addressing", "a"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not set namespace address for xmlNodeParam"));
        goto cleanup;
    }

    ns = NULL;
    if (!(ns = ws_xml_ns_add(xmlNodeParam,
                    "http://schemas.dmtf.org/wbem/wsman/1/wsman.xsd", "w"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not set wsman namespace address for xmlNodeParam"));
        goto cleanup;
    }

    if (xmlAddChild((xmlNodePtr) *parentNode, (xmlNodePtr) xmlNodeParam) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not add child to xml parent node"));
        goto cleanup;
    }

    if (xmlAddChild((xmlNodePtr) xmlNodeParam, xmlNodeAddrPtr) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not add child to xml parent node"));
        goto cleanup;
    }

    if (xmlAddChild((xmlNodePtr) xmlNodeParam, xmlNodeRefPtr) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not add child to xml parent node"));
        goto cleanup;
    }

    /* we did it! */
    result = 0;

cleanup:
    if (options != NULL)
        wsmc_options_destroy(options);
    if (filter != NULL)
        filter_destroy(filter);
    ws_xml_destroy_doc(xmlDocResponse);
    VIR_FREE(enumContext);
    VIR_FREE(query_string);
    return result;
}

/* Add an embedded param to the parent node passed in */
int
hypervAddEmbeddedParam(properties_t *prop_t, int nbProps, const char *paramName,
        const char *instanceName, const char *classURI, WsXmlNodeH *parentNode)
{
    int result = -1;
    WsXmlNodeH xmlNodeInstance = NULL,
               xmlNodeProperty = NULL,
               xmlNodeParam = NULL,
               xmlNodeArray = NULL;
    WsXmlDocH xmlDocTemp = NULL,
              xmlDocCdata = NULL;
    xmlBufferPtr xmlBufferNode = NULL;
    const xmlChar *xmlCharCdataContent = NULL;
    xmlNodePtr xmlNodeCdata = NULL;
    char *internalClassName = NULL;
    const char *type = NULL;
    bool isArray = false;
    int len = 0, i = 0;

    /* Add child to the parent */
    if (!(xmlNodeParam = ws_xml_add_child(*parentNode, classURI, paramName, NULL))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "Could not add child node %s",
                paramName);
        goto cleanup;
    }

    /* create the temp xml doc */

    /* start with the INSTANCE node */
    if (!(xmlDocTemp = ws_xml_create_doc(NULL, "INSTANCE"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not create temporary xml doc"));
        goto cleanup;
    }

    if (!(xmlNodeInstance = xml_parser_get_root(xmlDocTemp))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not get temp xml doc root"));
        goto cleanup;
    }

    /* get internal name from namespaced instanceName */
    if (VIR_STRNDUP(internalClassName, instanceName, strlen(instanceName) - 3) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not extract class name"));
        goto cleanup;
    }

    /* add CLASSNAME node to INSTANCE node */
    if (ws_xml_add_node_attr(xmlNodeInstance, NULL, "CLASSNAME",
                internalClassName) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not add attribute to node"));
        goto cleanup;
    }

    for (i = 0; i < nbProps; i++) {
        if (prop_t[i].name == NULL && prop_t[i].val == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("Could not get properties from array"));
            goto cleanup;
        }

        if (hypervGetPropType(instanceName, prop_t[i].name, &type, &isArray) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("Could not get properties from array"));
            goto cleanup;
        }

        if (!(xmlNodeProperty = ws_xml_add_child(xmlNodeInstance, NULL,
                        isArray ? "PROPERTY.ARRAY" : "PROPERTY", NULL))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("Could not add child to node"));
            goto cleanup;
        }

        if (ws_xml_add_node_attr(xmlNodeProperty, NULL, "NAME", prop_t[i].name) == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("Could not add attribute to node"));
            goto cleanup;
        }

        if (ws_xml_add_node_attr(xmlNodeProperty, NULL, "TYPE", type) == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("Could not add attribute to node"));
            goto cleanup;
        }

        /* If this attribute is an array, add the VALUE.ARARY node */
        if (isArray) {
            if (!(xmlNodeArray = ws_xml_add_child(xmlNodeProperty, NULL,
                            "VALUE.ARRAY", NULL))) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("Could not add child to node"));
                goto cleanup;
            }
        }

        /* add the child */
        if (ws_xml_add_child(isArray ? xmlNodeArray : xmlNodeProperty, NULL,
                    "VALUE", prop_t[i].val) == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("Could not add child to node"));
            goto cleanup;
        }

        xmlNodeArray = NULL;
        xmlNodeProperty = NULL;
    }

    /* create CDATA node */
    xmlBufferNode = xmlBufferCreate();
    if (xmlNodeDump(xmlBufferNode, (xmlDocPtr) xmlDocTemp->parserDoc,
                (xmlNodePtr) xmlNodeInstance, 0, 0) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not get root of temp xml doc"));
        goto cleanup;
    }

    len = xmlBufferLength(xmlBufferNode);
    xmlCharCdataContent = xmlBufferContent(xmlBufferNode);
    if (!(xmlNodeCdata = xmlNewCDataBlock((xmlDocPtr) xmlDocCdata,
                    xmlCharCdataContent, len))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not make CDATA"));
        goto cleanup;
    }

    /* Add CDATA node to the doc root */
    if (xmlAddChild((xmlNodePtr) xmlNodeParam, xmlNodeCdata) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not add CDATA to doc root"));
        goto cleanup;
    }

    /* we did it! */
    result = 0;

cleanup:
    VIR_FREE(internalClassName);
    ws_xml_destroy_doc(xmlDocCdata);
    ws_xml_destroy_doc(xmlDocTemp);
    if (!xmlBufferNode)
        xmlBufferFree(xmlBufferNode);
    return result;
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

      case MSVM_RETURNCODE_V1_FAILED:
        return _("Failed");

      case MSVM_RETURNCODE_V1_ACCESS_DENIED:
        return _("Access denied");

      case MSVM_RETURNCODE_V1_NOT_SUPPORTED:
        return _("Not supported");

      case MSVM_RETURNCODE_V1_STATUS_IS_UNKNOWN:
        return _("Status is unknown");

      case MSVM_RETURNCODE_V1_TIMEOUT:
        return _("Timeout");

      case MSVM_RETURNCODE_V1_INVALID_PARAMETER:
        return _("Invalid parameter");

      case MSVM_RETURNCODE_V1_SYSTEM_IS_IN_USE:
        return _("System is in use");

      case MSVM_RETURNCODE_V1_INVALID_STATE_FOR_THIS_OPERATION:
        return _("Invalid state for this operation");

      case MSVM_RETURNCODE_V1_INCORRECT_DATA_TYPE:
        return _("Incorrect data type");

      case MSVM_RETURNCODE_V1_SYSTEM_IS_NOT_AVAILABLE:
        return _("System is not available");

      case MSVM_RETURNCODE_V1_OUT_OF_MEMORY:
        return _("Out of memory");

      default:
        return _("Unknown return code");
    }
}


#include "hyperv_wmi.generated.c"
