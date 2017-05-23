/*
 * hyperv_api_v2.h: core driver functions for Hyper-V API version 2 hosts
 *
 * Copyright (C) 2017 Datto Inc.
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
#include <config.h>
#include <wsman-soap.h>
#include <fcntl.h>

#define VIR_FROM_THIS VIR_FROM_HYPERV

#include "hyperv_api_v2.h"

#include "virlog.h"
#include "virstring.h"
#include "virkeycode.h"
#include "fdstream.h"
#include "base64.h"

#include "hyperv_driver.h"
#include "hyperv_private.h"
#include "hyperv_util.h"
#include "hyperv_wmi.h"
#include "openwsman.h"

VIR_LOG_INIT("hyperv.hyperv_api_v2")

/*
 * WMI invocation functions
 *
 * functions for invoking WMI methods via SOAP
 */
static int
hyperv2InvokeMethodXml(hypervPrivate *priv, WsXmlDocH xmlDocRoot,
        const char *methodName, const char *resourceURI, const char *selector,
        WsXmlDocH *res)
{
    int result = -1;
    int returnCode;
    char *instanceID = NULL;
    char *xpath_expr_string = NULL;
    char *returnValue = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    virBuffer xpath_expr_buf = VIR_BUFFER_INITIALIZER;
    client_opt_t *options = NULL;
    WsXmlDocH response = NULL;
    Msvm_ConcreteJob_V2 *job = NULL;
    int jobState = -1;
    bool completed = false;

    options = wsmc_options_init();

    if (!options) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not init options"));
        goto cleanup;
    }

    wsmc_add_selectors_from_str(options, selector);

    /* Invoke action */
    response = wsmc_action_invoke(priv->client, resourceURI, options,
            methodName, xmlDocRoot);

    /* check return code of invocation */
    virBufferAsprintf(&xpath_expr_buf,
            "/s:Envelope/s:Body/p:%s_OUTPUT/p:ReturnValue", methodName);
    xpath_expr_string = virBufferContentAndReset(&xpath_expr_buf);
    if (!xpath_expr_string) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup ReturnValue for %s invocation"),
                       methodName);
        goto cleanup;
    }

    returnValue = ws_xml_get_xpath_value(response, xpath_expr_string);
    VIR_FREE(xpath_expr_string);
    xpath_expr_string = NULL;

    if (!returnValue) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup ReturnValue for %s invocation"),
                       methodName);
        hypervDebugResponseXml(response);
        goto cleanup;
    }

    if (virStrToLong_i(returnValue, NULL, 10, &returnCode) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not parse return code"));
        goto cleanup;
    }

    if (returnCode == CIM_RETURNCODE_TRANSITION_STARTED) {
        virBufferAsprintf(&xpath_expr_buf,
                "/s:Envelope/s:Body/p:%s_OUTPUT/p:Job/a:ReferenceParameters/"
                "w:SelectorSet/w:Selector[@Name='InstanceID']", methodName);

        if (virBufferCheckError(&xpath_expr_buf) < 0)
            goto cleanup;

        xpath_expr_string = virBufferContentAndReset(&xpath_expr_buf);

        /* Get Msvm_ConcreteJob_V2 object */
        instanceID = ws_xml_get_xpath_value(response, xpath_expr_string);
        if (!instanceID) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not look up instance ID for %s invocation"),
                           methodName);
            goto cleanup;
        }

        /* Poll every 100ms until the job completes or fails */
        while (!completed) {
            virBufferAddLit(&query, MSVM_CONCRETEJOB_V2_WQL_SELECT);
            virBufferAsprintf(&query, "where InstanceID = \"%s\"", instanceID);

            if (hyperv2GetMsvmConcreteJobList(priv, &query, &job) < 0)
                goto cleanup;

            if (!job) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Could not lookup ConcreteJob for %s invocation"),
                               methodName);
                goto cleanup;
            }

            /* do things depending on the state */
            jobState = job->data->JobState;
            switch (jobState) {
                case MSVM_CONCRETEJOB_V2_JOBSTATE_NEW:
                case MSVM_CONCRETEJOB_V2_JOBSTATE_STARTING:
                case MSVM_CONCRETEJOB_V2_JOBSTATE_RUNNING:
                case MSVM_CONCRETEJOB_V2_JOBSTATE_SHUTTING_DOWN:
                    hypervFreeObject(priv, (hypervObject *) job);
                    job = NULL;
                    usleep(100 * 1000); /* sleep 100 ms */
                    continue;
                case MSVM_CONCRETEJOB_V2_JOBSTATE_COMPLETED:
                    completed = true;
                    break;
                case MSVM_CONCRETEJOB_V2_JOBSTATE_TERMINATED:
                case MSVM_CONCRETEJOB_V2_JOBSTATE_KILLED:
                case MSVM_CONCRETEJOB_V2_JOBSTATE_EXCEPTION:
                case MSVM_CONCRETEJOB_V2_JOBSTATE_SERVICE:
                    goto cleanup;
                default:
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                            "Unknown state of invocation");
                    goto cleanup;
            }
        }
    } else if (returnCode != CIM_RETURNCODE_COMPLETED_WITH_NO_ERROR) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Invocation of %s returned an error: %s (%d)"),
                methodName, hypervReturnCodeToString(returnCode),
                returnCode);
        hypervDebugResponseXml(response);
        goto cleanup;
    }

    if (res != NULL)
        *res = response;

    result = 0;

cleanup:
    if (options)
        wsmc_options_destroy(options);
    if (response && (res == NULL))
        ws_xml_destroy_doc(response);
    VIR_FREE(returnValue);
    VIR_FREE(instanceID);
    VIR_FREE(xpath_expr_string);
    hypervFreeObject(priv, (hypervObject *) job);
    virBufferFreeAndReset(&query);
    virBufferFreeAndReset(&xpath_expr_buf);
    return result;
}

static int
hyperv2InvokeMethod(hypervPrivate *priv, invokeXmlParam *param_t, int nbParameters,
        const char *methodName, const char *providerURI, const char *selector,
        WsXmlDocH *res)
{
    int result = -1;
    WsXmlDocH doc = NULL;
    WsXmlNodeH methodNode = NULL;
    simpleParam *simple;
    eprParam *epr;
    embeddedParam *embedded;
    int i;

    if (hypervCreateXmlStruct(methodName, providerURI, &doc, &methodNode) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not create xml base structure"));
        goto cleanup;
    }

    /* Process and include parameters */
    for (i = 0; i < nbParameters; i++) {
        switch (param_t[i].type) {
            case SIMPLE_PARAM:
                simple = (simpleParam *) param_t[i].param;
                if (hypervAddSimpleParam(param_t[i].name, simple->value,
                            providerURI, &methodNode) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                            _("Could not add simple param"));
                    goto cleanup;
                }
                break;
            case EPR_PARAM:
                epr = (eprParam *) param_t[i].param;
                if (hypervAddEprParam(param_t[i].name, epr->query,
                            epr->wmiProviderURI, providerURI, &methodNode,
                            doc, priv) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                            _("Could not add epr param"));
                    goto cleanup;
                }
                break;
            case EMBEDDED_PARAM:
                embedded = (embeddedParam *) param_t[i].param;
                if (hypervAddEmbeddedParam(embedded->prop_t, embedded->nbProps,
                            param_t[i].name, embedded->instanceName,
                            providerURI, &methodNode) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                            _("Could not add embedded param"));
                    goto cleanup;
                }
                break;
            default:
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("Unknown parameter type"));
                goto cleanup;
        }
    }

    /* invoke the method */
    if (hyperv2InvokeMethodXml(priv, doc, methodName, providerURI, selector,
                res) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Error during invocation action"));
        goto cleanup;
    }

    result = 0;
cleanup:
    if (!doc)
        ws_xml_destroy_doc(doc);
    return result;
}

/*
 * WMI utility functions
 *
 * wrapper functions for commonly-accessed WMI objects and interfaces.
 */

static int
hyperv2GetProcessorsByName(hypervPrivate *priv, const char *name,
        Win32_Processor **processorList)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    /* Get Win32_Processor list */
    virBufferAsprintf(&query,
                      "associators of "
                      "{Win32_ComputerSystem.Name=\"%s\"} "
                      "where AssocClass = Win32_ComputerSystemProcessor "
                      "ResultClass = Win32_Processor",
                      name);

    if (hypervGetWin32ProcessorList(priv, &query, processorList) < 0)
        goto cleanup;

    if (processorList == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s"),
                       "Win32_Processor");
        goto cleanup;
    }
    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2GetActiveVirtualSystemList(hypervPrivate *priv,
        Msvm_ComputerSystem_V2 **computerSystemList)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_VIRTUAL);
    virBufferAddLit(&query, "and ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_ACTIVE);

    if (hyperv2GetMsvmComputerSystemList(priv, &query, computerSystemList) < 0)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

/* gets all the vms including the ones that are marked inactive. */
static int
hyperv2GetInactiveVirtualSystemList(hypervPrivate *priv,
        Msvm_ComputerSystem_V2 **list)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_VIRTUAL);
    virBufferAddLit(&query, "and ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_INACTIVE);

    if (hyperv2GetMsvmComputerSystemList(priv, &query, list) < 0)
        goto cleanup;

    if (*list == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2GetPhysicalSystemList(hypervPrivate *priv,
        Win32_ComputerSystem **computerSystemList)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, WIN32_COMPUTERSYSTEM_WQL_SELECT);

    if (hypervGetWin32ComputerSystemList(priv, &query, computerSystemList) < 0)
        goto cleanup;

    if (computerSystemList == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not lookup %s"),
                "Win32_ComputerSystem");
        goto cleanup;
    }

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2GetVirtualSystemByID(hypervPrivate *priv, int id,
        Msvm_ComputerSystem_V2 **computerSystemList)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and ProcessID = %d", id);

    if (hyperv2GetMsvmComputerSystemList(priv, &query, computerSystemList) < 0)
        goto cleanup;

    if (*computerSystemList == NULL)
        goto cleanup;

    result = 0;

cleanup:
    return result;
}

static int
hyperv2GetVirtualSystemByUUID(hypervPrivate *priv, const char *uuid,
        Msvm_ComputerSystem_V2 **computerSystemList)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and Name = \"%s\"", uuid);

    if (hyperv2GetMsvmComputerSystemList(priv, &query, computerSystemList) < 0)
        goto cleanup;

    if (*computerSystemList == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}


static int
hyperv2GetVirtualSystemByName(hypervPrivate *priv, const char *name,
        Msvm_ComputerSystem_V2 **computerSystemList)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and ElementName = \"%s\"", name);

    if (hyperv2GetMsvmComputerSystemList(priv, &query, computerSystemList) < 0)
        goto cleanup;

    if (*computerSystemList == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2GetVSSDFromUUID(hypervPrivate *priv, const char *uuid,
        Msvm_VirtualSystemSettingData_V2 **data)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    /* Get Msvm_VirtualSystemSettingData_V2 */
    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_ComputerSystem.CreationClassname=\"Msvm_ComputerSystem\","
            "Name=\"%s\"} "
            "where AssocClass = Msvm_SettingsDefineState "
            "ResultClass = Msvm_VirtualSystemSettingData",
            uuid);

    if (hyperv2GetMsvmVirtualSystemSettingDataList(priv, &query, data) < 0)
        goto cleanup;

    if (*data == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2GetProcSDByVSSDInstanceId(hypervPrivate *priv, const char *id,
        Msvm_ProcessorSettingData_V2 **data)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    /* Get Msvm_ProcessorSettingData_V2 */
    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
            "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
            "ResultClass = Msvm_ProcessorSettingData",
            id);

    if (hyperv2GetMsvmProcessorSettingDataList(priv, &query, data) < 0)
        goto cleanup;

    if (*data == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2GetMemSDByVSSDInstanceId(hypervPrivate *priv, const char *id,
        Msvm_MemorySettingData_V2 **data)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    /* Get Msvm_MemorySettingData_V2 */
    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
            "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
            "ResultClass = Msvm_MemorySettingData",
            id);

    if (hyperv2GetMsvmMemorySettingDataList(priv, &query, data) < 0)
        goto cleanup;

    if (*data == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2GetRASDByVSSDInstanceId(hypervPrivate *priv, const char *id,
        Msvm_ResourceAllocationSettingData_V2 **data)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
            "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
            "ResultClass = Msvm_ResourceAllocationSettingData",
            id);

    if (hyperv2GetMsvmResourceAllocationSettingDataList(priv, &query, data) < 0)
        goto cleanup;

    if (*data == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2GetSASDByVSSDInstanceId(hypervPrivate *priv, const char *id,
        Msvm_StorageAllocationSettingData_V2 **data)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
            "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
            "ResultClass = Msvm_StorageAllocationSettingData",
            id);

    if (hyperv2GetMsvmStorageAllocationSettingDataList(priv, &query, data) < 0)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2GetEthernetPortAllocationSDByVSSDInstanceId(hypervPrivate *priv,
        const char *id, Msvm_EthernetPortAllocationSettingData_V2 **out)
{
    int result = -1;
    virBuffer query = VIR_BUFFER_INITIALIZER;

    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
            "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
            "ResultClass = Msvm_EthernetPortAllocationSettingData",
            id);

    if (hyperv2GetMsvmEthernetPortAllocationSettingDataList(priv, &query,
                out) < 0)
        goto cleanup;

    if (*out == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

/* API-specific utility functions */
static int
hyperv2LookupHostSystemBiosUuid(hypervPrivate *priv, unsigned char *uuid)
{
    Win32_ComputerSystemProduct *computerSystem = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, WIN32_COMPUTERSYSTEMPRODUCT_WQL_SELECT);
    if (hypervGetWin32ComputerSystemProductList(priv, &query,
                &computerSystem) < 0) {
        goto cleanup;
    }

    if (virUUIDParse(computerSystem->data->UUID, uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not parse UUID from string '%s'"),
                computerSystem->data->UUID);
        goto cleanup;
    }
    result = 0;

cleanup:
    hypervFreeObject(priv, (hypervObject *) computerSystem);
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2GetHostSystem(hypervPrivate *priv, Msvm_ComputerSystem_V2 **system)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_PHYSICAL);

    if (hyperv2GetMsvmComputerSystemList(priv, &query, system) < 0)
        goto cleanup;

    if (*system == NULL)
        goto cleanup;

    result = 0;
cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2InvokeMsvmComputerSystemRequestStateChange(virDomainPtr domain,
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
    Msvm_ConcreteJob_V2 *concreteJob = NULL;
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
    response = wsmc_action_invoke(priv->client, MSVM_COMPUTERSYSTEM_V2_RESOURCE_URI,
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
            virBufferAddLit(&query, MSVM_CONCRETEJOB_V2_WQL_SELECT);
            virBufferAsprintf(&query, "where InstanceID = \"%s\"", instanceID);

            if (hyperv2GetMsvmConcreteJobList(priv, &query, &concreteJob) < 0)
                goto cleanup;

            if (concreteJob == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Could not lookup %s for %s invocation"),
                               "Msvm_ConcreteJob", "RequestStateChange");
                goto cleanup;
            }

            switch (concreteJob->data->JobState) {
              case MSVM_CONCRETEJOB_V2_JOBSTATE_NEW:
              case MSVM_CONCRETEJOB_V2_JOBSTATE_STARTING:
              case MSVM_CONCRETEJOB_V2_JOBSTATE_RUNNING:
              case MSVM_CONCRETEJOB_V2_JOBSTATE_SHUTTING_DOWN:
                hypervFreeObject(priv, (hypervObject *)concreteJob);
                concreteJob = NULL;

                usleep(100 * 1000);
                continue;

              case MSVM_CONCRETEJOB_V2_JOBSTATE_COMPLETED:
                completed = true;
                break;

              case MSVM_CONCRETEJOB_V2_JOBSTATE_TERMINATED:
              case MSVM_CONCRETEJOB_V2_JOBSTATE_KILLED:
              case MSVM_CONCRETEJOB_V2_JOBSTATE_EXCEPTION:
              case MSVM_CONCRETEJOB_V2_JOBSTATE_SERVICE:
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

static int
hyperv2MsvmComputerSystemEnabledStateToDomainState
  (Msvm_ComputerSystem_V2 *computerSystem)
{
    switch (computerSystem->data->EnabledState) {
      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_UNKNOWN:
        return VIR_DOMAIN_NOSTATE;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_ENABLED:
        return VIR_DOMAIN_RUNNING;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_DISABLED:
        return VIR_DOMAIN_SHUTOFF;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_PAUSED:
        return VIR_DOMAIN_PAUSED;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SUSPENDED: /* managed save */
        return VIR_DOMAIN_SHUTOFF;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_STARTING:
      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SNAPSHOTTING:
      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SAVING:
        return VIR_DOMAIN_RUNNING;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_STOPPING:
        return VIR_DOMAIN_SHUTDOWN;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_PAUSING:
      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_RESUMING:
        return VIR_DOMAIN_RUNNING;

      default:
        return VIR_DOMAIN_NOSTATE;
    }
}

static bool
hyperv2IsMsvmComputerSystemActive(Msvm_ComputerSystem_V2 *computerSystem,
                                 bool *in_transition)
{
    if (in_transition != NULL)
        *in_transition = false;

    switch (computerSystem->data->EnabledState) {
      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_UNKNOWN:
        return false;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_ENABLED:
        return true;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_DISABLED:
        return false;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_PAUSED:
        return true;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SUSPENDED: /* managed save */
        return false;

      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_STARTING:
      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SNAPSHOTTING:
      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SAVING:
      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_STOPPING:
      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_PAUSING:
      case MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_RESUMING:
        if (in_transition != NULL)
            *in_transition = true;

        return true;

      default:
        return false;
    }
}

static int
hyperv2MsvmComputerSystemToDomain(virConnectPtr conn,
                                 Msvm_ComputerSystem_V2 *computerSystem,
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

    if (hyperv2IsMsvmComputerSystemActive(computerSystem, NULL)) {
        (*domain)->id = computerSystem->data->ProcessID;
    } else {
        (*domain)->id = -1;
    }

    return 0;
}

static int
hyperv2MsvmComputerSystemFromDomain(virDomainPtr domain,
                                   Msvm_ComputerSystem_V2 **computerSystem)
{
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;

    if (computerSystem == NULL || *computerSystem != NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid argument"));
        return -1;
    }

    virUUIDFormat(domain->uuid, uuid_string);

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and Name = \"%s\"", uuid_string);

    if (hyperv2GetMsvmComputerSystemList(priv, &query, computerSystem) < 0)
        return -1;

    if (*computerSystem == NULL) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("No domain with UUID %s"), uuid_string);
        return -1;
    }

    return 0;
}

/* General-purpose utility functions */
virCapsPtr
hyperv2CapsInit(hypervPrivate *priv)
{
    virCapsPtr caps = NULL;
    virCapsGuestPtr guest = NULL;

    caps = virCapabilitiesNew(VIR_ARCH_X86_64, 1, 1);

    if (caps == NULL) {
        virReportOOMError();
        return NULL;
    }

    if (hyperv2LookupHostSystemBiosUuid(priv, caps->host.host_uuid) < 0)
        goto error;

    /* i686 caps */
    guest = virCapabilitiesAddGuest(caps, VIR_DOMAIN_OSTYPE_HVM, VIR_ARCH_I686,
            NULL, NULL, 0, NULL);
    if (guest == NULL)
        goto error;

    if (virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_HYPERV, NULL, NULL,
                0, NULL) == NULL)
        goto error;

    /* x86_64 caps */
    guest = virCapabilitiesAddGuest(caps, VIR_DOMAIN_OSTYPE_HVM, VIR_ARCH_X86_64,
            NULL, NULL, 0, NULL);
    if (guest == NULL)
        goto error;

    if (virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_HYPERV, NULL, NULL,
                0, NULL) == NULL)
        goto error;

    return caps;

error:
    virObjectUnref(caps);
    return NULL;
}

/* Virtual device functions */
static int
hyperv2GetDeviceParentRasdFromDeviceId(const char *parentDeviceId,
        Msvm_ResourceAllocationSettingData_V2 *list,
        Msvm_ResourceAllocationSettingData_V2 **out)
{
    int result = -1;
    Msvm_ResourceAllocationSettingData_V2 *entry = list;
    char *escapedDeviceId = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    *out = NULL;

    while (entry != NULL) {
        virBufferAsprintf(&buf, "%s\"", entry->data->InstanceID);
        escapedDeviceId = virBufferContentAndReset(&buf);
        escapedDeviceId = virStringReplace(escapedDeviceId, "\\", "\\\\");

        if (virStringEndsWith(parentDeviceId, escapedDeviceId)) {
            *out = entry;
            break;
        }
        entry = entry->next;
    }

    if (*out != NULL)
        result = 0;

    return result;
}

static char *
hyperv2GetInstanceIDFromXMLResponse(WsXmlDocH response)
{
    WsXmlNodeH envelope = NULL;
    char *instanceId = NULL;

    envelope = ws_xml_get_soap_envelope(response);
    if (!envelope) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Invalid XML response"));
        goto cleanup;
    }

    instanceId = ws_xml_get_xpath_value(response,
            (char *) "//w:Selector[@Name='InstanceID']");

    if (!instanceId) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not find selectors in method response"));
        goto cleanup;
    }

    return instanceId;

cleanup:
    return NULL;
}

/* Functions for deserializing device entries */
static int
hyperv2DomainDefParseIDEController(virDomainDefPtr def,
        Msvm_ResourceAllocationSettingData_V2 *ide ATTRIBUTE_UNUSED, int idx)
{
    int result = -1;
    virDomainControllerDefPtr ctrlr = NULL;

    ctrlr = virDomainControllerDefNew(VIR_DOMAIN_CONTROLLER_TYPE_IDE);
    if (ctrlr == NULL)
        goto cleanup;

    ctrlr->idx = idx;

    if (VIR_APPEND_ELEMENT(def->controllers, def->ncontrollers, ctrlr) < 0)
        goto cleanup;

    result = 0;
cleanup:
    return result;
}

static int
hyperv2DomainDefParseSCSIController(virDomainDefPtr def,
        Msvm_ResourceAllocationSettingData_V2 *scsi ATTRIBUTE_UNUSED, int idx)
{
    int result = -1;
    virDomainControllerDefPtr ctrlr = NULL;

    ctrlr = virDomainControllerDefNew(VIR_DOMAIN_CONTROLLER_TYPE_SCSI);
    if (ctrlr == NULL)
        goto cleanup;

    ctrlr->idx = idx;

    if (VIR_APPEND_ELEMENT(def->controllers, def->ncontrollers, ctrlr) < 0)
        goto cleanup;

    result = 0;

cleanup:
    return result;
}

static int
hyperv2DomainDefParseIDEStorageExtent(virDomainDefPtr def, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V2 **ideControllers,
        Msvm_ResourceAllocationSettingData_V2 *disk_parent,
        Msvm_ResourceAllocationSettingData_V2 *disk_ctrlr)
{
    int i = 0;
    int result = -1;
    int ctrlr_idx = -1;
    int addr = -1;

    /* Find controller index */
    for (i = 0; i < HYPERV2_MAX_IDE_CONTROLLERS; i++) {
        if (disk_ctrlr == ideControllers[i]) {
            ctrlr_idx = i;
            break;
        }
    }
    if (ctrlr_idx < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                "Could not find controller for disk!");
        goto cleanup;
    }

    addr = atoi(disk_parent->data->AddressOnParent);
    if (addr < 0)
        goto cleanup;

    disk->bus = VIR_DOMAIN_DISK_BUS_IDE;
    disk->dst = virIndexToDiskName(ctrlr_idx * 4 + addr, "hd");
    disk->info.addr.drive.controller = ctrlr_idx;
    disk->info.addr.drive.bus = 0;
    disk->info.addr.drive.target = 0;
    disk->info.addr.drive.unit = addr;

    if (VIR_APPEND_ELEMENT(def->disks, def->ndisks, disk) < 0)
        goto cleanup;

    result = 0;

cleanup:
    return result;
}

static int
hyperv2DomainDefParseSCSIStorageExtent(virDomainDefPtr def, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V2 **scsiControllers,
        Msvm_ResourceAllocationSettingData_V2 *disk_parent,
        Msvm_ResourceAllocationSettingData_V2 *disk_ctrlr)
{
    int i = 0;
    int ctrlr_idx = -1;
    int result = -1;
    int addr = -1;

    /* Find controller index */
    for (i = 0; i < HYPERV2_MAX_SCSI_CONTROLLERS; i++) {
        if (disk_ctrlr == scsiControllers[i]) {
            ctrlr_idx = i;
            break;
        }
    }
    if (ctrlr_idx < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                "Could not find controller for disk!");
        goto cleanup;
    }

    addr = atoi(disk_parent->data->AddressOnParent);
    if (addr < 0)
        goto cleanup;

    disk->bus = VIR_DOMAIN_DISK_BUS_SCSI;
    disk->dst = virIndexToDiskName(ctrlr_idx * 64 + addr, "sd");
    disk->info.addr.drive.controller = ctrlr_idx;
    disk->info.addr.drive.bus = 0;
    disk->info.addr.drive.target = 0;
    disk->info.addr.drive.unit = addr;

    if (VIR_APPEND_ELEMENT(def->disks, def->ndisks, disk) < 0)
        goto cleanup;

    result = 0;

cleanup:
    return result;
}

static int
hyperv2DomainDefParseFloppyStorageExtent(virDomainDefPtr def, virDomainDiskDefPtr disk)
{
    int result = -1;
    /* Parse floppy drive */
    disk->bus = VIR_DOMAIN_DISK_BUS_FDC;
    if (VIR_STRDUP(disk->dst, "fda") < 0)
        goto cleanup;

    if (VIR_APPEND_ELEMENT(def->disks, def->ndisks, disk) < 0)
        goto cleanup;

    result = 0;

cleanup:
    return result;
}

static int
hyperv2DomainDefParseStorage(virDomainPtr domain, virDomainDefPtr def,
        Msvm_ResourceAllocationSettingData_V2 *rasd,
        Msvm_StorageAllocationSettingData_V2 *sasd)
{
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ResourceAllocationSettingData_V2 *entry = rasd;
    Msvm_ResourceAllocationSettingData_V2 *disk_parent = NULL, *disk_ctrlr = NULL;
    Msvm_StorageAllocationSettingData_V2 *disk_entry = sasd;
    virDomainDiskDefPtr disk = NULL;
    int result = -1;
    int scsi_idx = 0;
    int ide_idx = -1;
    char **matches = NULL;
    char **hostResource = NULL;
    char *hostEscaped = NULL;
    char *driveNumberStr = NULL;
    Msvm_DiskDrive_V2 *diskdrive = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int addr = -1, i = 0, ctrlr_idx = -1;
    Msvm_ResourceAllocationSettingData_V2 *ideControllers[HYPERV2_MAX_IDE_CONTROLLERS];
    Msvm_ResourceAllocationSettingData_V2 *scsiControllers[HYPERV2_MAX_SCSI_CONTROLLERS];

    while (entry != NULL) {
        switch (entry->data->ResourceType) {
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_IDE_CONTROLLER:
                ide_idx = entry->data->Address[0] - '0';
                ideControllers[ide_idx] = entry;
                if (hyperv2DomainDefParseIDEController(def, entry, ide_idx) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "Could not parse IDE controller");
                    goto cleanup;
                }
                break;
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_PARALLEL_SCSI_HBA:
                scsiControllers[scsi_idx++] = entry;
                if (hyperv2DomainDefParseSCSIController(def, entry, scsi_idx-1) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "Could not parse SCSI controller");
                    goto cleanup;
                }
                break;
            default:
                /* do nothing for now */
                break;
        }
        entry = entry->next;
    }

    /* second pass to parse physical disks */
    entry = rasd;
    while (entry != NULL) {
        if (entry->data->ResourceType ==
                MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_DISK) {
            /* code to parse physical disk drives, i.e. LUNs */
            if (entry->data->HostResource.count > 0) {
                /* clear some vars */
                disk = NULL;
                diskdrive = NULL;

                hostResource = entry->data->HostResource.data;
                if (strstr(*hostResource, "NODRIVE")) {
                    /* Hyper-V doesn't let you define LUNs with no connection */
                    VIR_DEBUG("Skipping empty LUN '%s'", *hostResource);
                    entry = entry->next;
                    continue;
                }

                if (hyperv2GetDeviceParentRasdFromDeviceId(entry->data->Parent,
                            rasd, &disk_ctrlr) < 0)
                    goto cleanup;

                /* create disk definition */
                if (!(disk = virDomainDiskDefNew(priv->xmlopt))) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "Could not allocate disk def");
                    goto cleanup;
                }

                /* Query Msvm_DiskDrive for the DriveNumber */
                hostEscaped = virStringReplace(*hostResource,
                        "\\", "\\\\");
                hostEscaped = virStringReplace(hostEscaped, "\"", "\\\"");

                virBufferAsprintf(&query,
                        "select * from Msvm_DiskDrive where "
                        "__PATH=\"%s\"", hostEscaped);

                if (hyperv2GetMsvmDiskDriveList(priv, &query, &diskdrive) < 0
                        || diskdrive == NULL) {
                    VIR_DEBUG("Didn't work; hostResource is %s", hostEscaped);
                    goto cleanup;
                }

                driveNumberStr = virNumToStr(diskdrive->data->DriveNumber);
                ignore_value(virDomainDiskSetSource(disk, driveNumberStr));

                addr = atoi(entry->data->AddressOnParent);
                if (addr < 0)
                    goto cleanup;

                switch (disk_ctrlr->data->ResourceType) {
                    case MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_PARALLEL_SCSI_HBA:
                        for (i = 0; i < HYPERV2_MAX_SCSI_CONTROLLERS; i++) {
                            if (disk_ctrlr == scsiControllers[i]) {
                                ctrlr_idx = i;
                                break;
                            }
                        }
                        disk->bus = VIR_DOMAIN_DISK_BUS_SCSI;
                        disk->dst = virIndexToDiskName(ctrlr_idx * 64 + addr, "sd");
                        disk->info.addr.drive.unit = addr;
                        break;
                    case MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_IDE_CONTROLLER:
                        for (i = 0; i < HYPERV2_MAX_IDE_CONTROLLERS; i++) {
                            if (disk_ctrlr == ideControllers[i]) {
                                ctrlr_idx = i;
                                break;
                            }
                        }
                        disk->bus = VIR_DOMAIN_DISK_BUS_IDE;
                        disk->dst = virIndexToDiskName(ctrlr_idx * 4 + addr, "hd");
                        disk->info.addr.drive.unit = addr;
                        break;
                    default:
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                _("Invalid controller type for LUN"));
                        goto cleanup;
                }

                disk->info.addr.drive.controller = ctrlr_idx;
                disk->info.addr.drive.bus = 0;
                disk->info.addr.drive.target = 0;
                virDomainDiskSetType(disk, VIR_STORAGE_TYPE_BLOCK);
                disk->device = VIR_DOMAIN_DISK_DEVICE_DISK;

                disk->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;

                if (VIR_APPEND_ELEMENT(def->disks, def->ndisks, disk) < 0)
                    goto cleanup;
            }
        }
        entry = entry->next;
    }

    while (disk_entry != NULL) {
        if (!(disk = virDomainDiskDefNew(priv->xmlopt))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("Could not allocate disk definition"));
            goto cleanup;
        }

        /* get disk associated with storage extent */
        if (hyperv2GetDeviceParentRasdFromDeviceId(disk_entry->data->Parent,
                    rasd, &disk_parent) < 0)
            goto cleanup;

        /* get associated controller */
        if (hyperv2GetDeviceParentRasdFromDeviceId(disk_parent->data->Parent,
                    rasd, &disk_ctrlr) < 0)
            goto cleanup;

        /* common fields first */
        disk->src->type = VIR_STORAGE_TYPE_FILE;
        disk->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;

        /* note if it's a CDROM disk */
        if (STREQ(disk_entry->data->ResourceSubType,
                    "Microsoft:Hyper-V:Virtual CD/DVD Disk")) {
            disk->device = VIR_DOMAIN_DISK_DEVICE_CDROM;
        } else {
            disk->device = VIR_DOMAIN_DISK_DEVICE_DISK;
        }

        /* copy in the source path */
        if (disk_entry->data->HostResource.count < 1)
            goto cleanup; /* TODO: maybe don't abort here? */
        if (virDomainDiskSetSource(disk,
                    *((char **) disk_entry->data->HostResource.data)) < 0) {
            goto cleanup;
        }

        /* controller-specific fields */
        switch (disk_ctrlr->data->ResourceType) {
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_PARALLEL_SCSI_HBA:
                if (hyperv2DomainDefParseSCSIStorageExtent(def, disk,
                        scsiControllers, disk_parent, disk_ctrlr) < 0) {
                    goto cleanup;
                }
                break;
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_IDE_CONTROLLER:
                if (hyperv2DomainDefParseIDEStorageExtent(def, disk,
                            ideControllers, disk_parent, disk_ctrlr) < 0) {
                    goto cleanup;
                }
                break;
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_OTHER:
                if (disk_parent->data->ResourceType ==
                        MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_FLOPPY) {
                    if (hyperv2DomainDefParseFloppyStorageExtent(def, disk) < 0)
                        goto cleanup;
                    disk->device = VIR_DOMAIN_DISK_DEVICE_FLOPPY;
                }
                break;
            default:
                virReportError(VIR_ERR_INTERNAL_ERROR,
                        "Unrecognized controller type %d",
                        disk_ctrlr->data->ResourceType);
                goto cleanup;
        }


        disk_entry = disk_entry->next;
    }

    result = 0;

cleanup:
    if (result != 0 && disk)
            virDomainDiskDefFree(disk);
    virStringListFree(matches);
    hypervFreeObject(priv, (hypervObject *) diskdrive);
    VIR_FREE(hostEscaped);
    VIR_FREE(driveNumberStr);
    virBufferFreeAndReset(&query);

    return result;
}

static int
hyperv2DomainDefParseEthernetAdapter(virDomainDefPtr def,
        Msvm_EthernetPortAllocationSettingData_V2 *net,
        hypervPrivate *priv)
{
    int result = -1;
    virDomainNetDefPtr ndef = NULL;
    Msvm_SyntheticEthernetPortSettingData_V2 *sepsd = NULL;
    Msvm_VirtualEthernetSwitch_V2 *vSwitch = NULL;
    char **switchConnection = NULL;
    char *switchConnectionEscaped = NULL;
    char *sepsdPATH = NULL;
    char *sepsdEscaped = NULL;
    char *temp = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;

    VIR_DEBUG("Parsing ethernet adapter '%s'", net->data->InstanceID);

    if (VIR_ALLOC(ndef) < 0)
        goto cleanup;

    ndef->type = VIR_DOMAIN_NET_TYPE_BRIDGE;

    /*
     * If there's no switch port connection or the EnabledState is disabled,
     * then the adapter isn't hooked up to anything and we don't have to
     * do anything more.
     */
    switchConnection = net->data->HostResource.data;
    if (net->data->HostResource.count < 1 || *switchConnection == NULL ||
            net->data->EnabledState == MSVM_ETHERNETPORTALLOCATIONSETTINGDATA_V2_ENABLEDSTATE_DISABLED) {
        VIR_DEBUG("Adapter not connected to switch");
        goto success;
    }

    /*
     * Now we retrieve the associated Msvm_SyntheticEthernetPortSettingData_V2
     * and Msvm_VirtualSwitch_V2 objects, and use all three to build the XML
     * definition.
     */

    /* begin by getting the Msvm_SyntheticEthernetPortSettingData_V2 object */
    sepsdPATH = net->data->Parent;
    sepsdEscaped = virStringReplace(sepsdPATH, "\\", "\\\\");
    sepsdEscaped = virStringReplace(sepsdEscaped, "\"", "\\\"");
    virBufferAsprintf(&query,
            "select * from Msvm_SyntheticEthernetPortSettingData "
            "where __PATH=\"%s\"",
            sepsdEscaped);

    if ((hyperv2GetMsvmSyntheticEthernetPortSettingDataList(priv, &query,
                &sepsd) < 0) || sepsd == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not retrieve settings"));
        goto cleanup;
    }

    /* set mac address */
    if (virMacAddrParseHex(sepsd->data->Address, &ndef->mac) < 0)
        goto cleanup;

    /* now we get the Msvm_VirtualEthernetSwitch_V2 */
    virBufferFreeAndReset(&query);
    switchConnectionEscaped = virStringReplace(*switchConnection,
            "\\", "\\\\");
    switchConnectionEscaped = virStringReplace(switchConnectionEscaped,
            "\"", "\\\"");

    virBufferAsprintf(&query,
                      "select * from Msvm_VirtualEthernetSwitch "
                      "where __PATH=\"%s\"",
                      switchConnectionEscaped);

    if (hyperv2GetMsvmVirtualEthernetSwitchList(priv, &query, &vSwitch) < 0 ||
            vSwitch == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not retrieve virtual switch"));
        goto cleanup;
    }

    /* get bridge name */
    if (VIR_STRDUP(temp, vSwitch->data->Name) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not set bridge name"));
        goto cleanup;
    }
    ndef->data.bridge.brname = temp;

    if (VIR_APPEND_ELEMENT(def->nets, def->nnets, ndef) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not append definition to domain"));
        goto cleanup;
    }

success:
    result = 0;

cleanup:
    hypervFreeObject(priv, (hypervObject *) sepsd);
    hypervFreeObject(priv, (hypervObject *) vSwitch);
    VIR_FREE(switchConnectionEscaped);
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv2DomainDefParseEthernet(virDomainPtr domain, virDomainDefPtr def,
        Msvm_EthernetPortAllocationSettingData_V2 *nets)
{
    int result = -1;
    Msvm_EthernetPortAllocationSettingData_V2 *entry = nets;
    hypervPrivate *priv = domain->conn->privateData;

    while (entry != NULL) {
        if (hyperv2DomainDefParseEthernetAdapter(def, entry, priv) < 0)
            goto cleanup;

        entry = entry->next;
    }

    result = 0;

cleanup:
    return result;
}

static int
hyperv2DomainDefParseSerial(virDomainPtr domain ATTRIBUTE_UNUSED,
        virDomainDefPtr def, Msvm_ResourceAllocationSettingData_V2 *rasd)
{
    int result = -1;
    int port_num = 0;
    char **conn = NULL;
    const char *srcPath = NULL;
    Msvm_ResourceAllocationSettingData_V2 *entry = rasd;
    virDomainChrDefPtr serial = NULL;

    while (entry != NULL) {
        if (entry->data->ResourceType ==
                MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_SERIAL_PORT) {
            /* clear some vars */
            serial = NULL;
            port_num = 0;
            conn = NULL;
            srcPath = NULL;

            /* get port number */
            port_num = entry->data->ElementName[4] - '0';
            if (port_num < 1)
                goto next;

            serial = virDomainChrDefNew(NULL);

            serial->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL;
            serial->source->type = VIR_DOMAIN_CHR_TYPE_PIPE;
            serial->target.port = port_num;

            /* set up source */
            if (entry->data->Connection.count < 1) {
                srcPath = "-1";
            } else {
                conn = entry->data->Connection.data;
                if (*conn == NULL)
                    srcPath = "-1";
                else
                    srcPath = *conn;
            }

            if (VIR_STRDUP(serial->source->data.file.path, srcPath) < 0)
                goto cleanup;

            if (VIR_APPEND_ELEMENT(def->serials, def->nserials, serial) < 0) {
                virDomainChrDefFree(serial);
                goto cleanup;
            }
        }
next:
        entry = entry->next;
    }

    result = 0;
cleanup:
    return result;
}

/* Functions for creating and attaching virtual devices */
static int
hyperv2DomainAttachSyntheticEthernetAdapter(virDomainPtr domain,
        virDomainNetDefPtr net, char *hostname)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    char guid_string[VIR_UUID_STRING_BUFLEN];
    char mac_string[VIR_MAC_STRING_BUFLEN];
    unsigned char vsi_guid[VIR_UUID_BUFLEN];
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    Msvm_VirtualEthernetSwitch_V2 *vSwitch = NULL;
    embeddedParam sepsd_embedded, epasd_embedded;
    char *switch__PATH = NULL;
    char *sepsd__PATH = NULL;
    char *sepsd_instance_escaped = NULL;
    char *sepsd_instance = NULL;
    char *virtualSystemIdentifiers = NULL;
    char *macAddrEscaped = NULL;
    eprParam vssd_REF;
    properties_t *props = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    invokeXmlParam *params = NULL;
    WsXmlDocH sepsd_doc = NULL;

    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    VIR_DEBUG("Stage 0");

    /*
     * Step 1: Create the Msvm_SyntheticEthernetPortSettingData_V2 object
     * that holds half the settings for the new adapter we are creating
     */
    virUUIDGenerate(vsi_guid);
    virUUIDFormat(vsi_guid, guid_string);
    if (virAsprintf(&virtualSystemIdentifiers, "{%s}", guid_string) < 0)
        goto cleanup;
    virMacAddrFormat(&net->mac, mac_string);
    macAddrEscaped = virStringReplace(mac_string, ":", "");

    sepsd_embedded.nbProps = 6;
    if (VIR_ALLOC_N(props, sepsd_embedded.nbProps) < 0)
        goto cleanup;
    props[0].name = "ResourceType";
    props[0].val = "10";
    props[1].name = "ResourceSubType";
    props[1].val = "Microsoft:Hyper-V:Synthetic Ethernet Port";
    props[2].name = "ElementName";
    props[2].val = "Network Adapter";
    props[3].name = "VirtualSystemIdentifiers";
    props[3].val = virtualSystemIdentifiers;
    props[4].name = "Address";
    props[4].val = macAddrEscaped;
    props[5].name = "StaticMacAddress";
    props[5].val = "true";
    sepsd_embedded.instanceName = MSVM_SYNTHETICETHERNETPORTSETTINGDATA_V2_CLASSNAME;
    sepsd_embedded.prop_t = props;

    virBufferAddLit(&query, MSVM_VIRTUALSYSTEMSETTINGDATA_V2_WQL_SELECT);
    virBufferAsprintf(&query, "where InstanceID=\"%s\"", vssd->data->InstanceID);
    vssd_REF.query = &query;
    vssd_REF.wmiProviderURI = ROOT_VIRTUALIZATION_V2;

    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "AffectedConfiguration";
    params[0].type = EPR_PARAM;
    params[0].param = &vssd_REF;
    params[1].name = "ResourceSettings";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &sepsd_embedded;

    if (hyperv2InvokeMethod(priv, params, 2, "AddResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                selector, &sepsd_doc) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not attach network"));
        goto cleanup;
    }

    /*
     * Step 2: Get the Msvm_VirtualEthernetSwitch_V2 object
     */
    virBufferFreeAndReset(&query);
    virBufferAddLit(&query, MSVM_VIRTUALETHERNETSWITCH_V2_WQL_SELECT);
    virBufferAsprintf(&query, " where Name=\"%s\"", net->data.bridge.brname);

    if (hyperv2GetMsvmVirtualEthernetSwitchList(priv, &query, &vSwitch) < 0
            || vSwitch == NULL)
        goto cleanup;

    /*
     * Step 3: Create the Msvm_EthernetPortAllocationSettingData object that
     * holds the other half of the network configuration
     */
    VIR_FREE(props);
    VIR_FREE(params);
    virBufferFreeAndReset(&query);

    /* build the two __PATH variables */
    if (virAsprintf(&switch__PATH, "\\\\%s\\root\\virtualization\\v2:"
                "Msvm_VirtualEthernetSwitch.CreationClassName="
                "\"Msvm_VirtualEthernetSwitch\",Name=\"%s\"",
                hostname, vSwitch->data->Name) < 0)
        goto cleanup;

    /* Get the sepsd instance ID out of the XML response */
    sepsd_instance = hyperv2GetInstanceIDFromXMLResponse(sepsd_doc);
    sepsd_instance_escaped = virStringReplace(sepsd_instance, "\\", "\\\\");
    if (virAsprintf(&sepsd__PATH, "\\\\%s\\root\\virtualization\\v2:"
                "Msvm_SyntheticEthernetPortSettingData.InstanceID=\"%s\"",
                hostname, sepsd_instance_escaped) < 0)
        goto cleanup;

    virBufferAddLit(&query, MSVM_VIRTUALSYSTEMSETTINGDATA_V2_WQL_SELECT);
    virBufferAsprintf(&query, "where InstanceID=\"%s\"", vssd->data->InstanceID);

    epasd_embedded.nbProps = 6;
    if (VIR_ALLOC_N(props, epasd_embedded.nbProps) < 0)
        goto cleanup;
    props[0].name = "EnabledState";
    props[0].val = "2";
    props[1].name = "HostResource";
    props[1].val = switch__PATH;
    props[2].name = "Parent";
    props[2].val = sepsd__PATH;
    props[3].name = "ResourceType";
    props[3].val = "33";
    props[4].name = "ResourceSubType";
    props[4].val = "Microsoft:Hyper-V:Ethernet Connection";
    props[5].name = "ElementName";
    props[5].val = "Dynamic Ethernet Switch Port";
    epasd_embedded.instanceName = MSVM_ETHERNETPORTALLOCATIONSETTINGDATA_V2_CLASSNAME;
    epasd_embedded.prop_t = props;

    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "AffectedConfiguration";
    params[0].type = EPR_PARAM;
    params[0].param = &vssd_REF;
    params[1].name = "ResourceSettings";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &epasd_embedded;

    if (hyperv2InvokeMethod(priv, params, 2, "AddResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not attach network"));
        goto cleanup;
    }

    result = 0;

cleanup:
    hypervFreeObject(priv, (hypervObject *) vSwitch);
    hypervFreeObject(priv, (hypervObject *) vssd);
    VIR_FREE(switch__PATH);
    VIR_FREE(sepsd__PATH);
    VIR_FREE(sepsd_instance_escaped);
    VIR_FREE(sepsd_instance);
    VIR_FREE(props);
    VIR_FREE(virtualSystemIdentifiers);
    VIR_FREE(macAddrEscaped);
    virBufferFreeAndReset(&query);
    VIR_FREE(params);
    if (sepsd_doc)
        ws_xml_destroy_doc(sepsd_doc);

    return result;
}

static int
hyperv2DomainAttachSerial(virDomainPtr domain, virDomainChrDefPtr serial)
{
    int result = -1;
    const char *selector = "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    char *com_string = NULL;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    Msvm_ResourceAllocationSettingData_V2 *rasd = NULL;
    Msvm_ResourceAllocationSettingData_V2 *entry = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    embeddedParam ResourceSettingData;
    properties_t *props = NULL;
    invokeXmlParam *params = NULL;

    if (virAsprintf(&com_string, "COM %d", serial->target.port) < 0)
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv2GetRASDByVSSDInstanceId(priv, vssd->data->InstanceID, &rasd) < 0)
        goto cleanup;

    /* find the COM port we're interested in changing */
    entry = rasd;
    while (entry != NULL) {
        if ((entry->data->ResourceType ==
                MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_SERIAL_PORT) &&
                STREQ(entry->data->ElementName, com_string)) {
            /* found our com port */
            break;
        }
        entry = entry->next;
    }

    if (entry == NULL)
        goto cleanup;

    /* build rasd param */
    ResourceSettingData.nbProps = 4;
    if (VIR_ALLOC_N(props, ResourceSettingData.nbProps) < 0)
        goto cleanup;
    props[0].name = "Connection";
    if (STRNEQ(serial->source->data.file.path, "-1"))
        props[0].val = serial->source->data.file.path;
    else
        props[0].val = "";
    props[1].name = "InstanceID";
    props[1].val = entry->data->InstanceID;
    props[2].name = "ResourceType";
    props[2].val = "21"; /* shouldn't be hardcoded but whatever */
    props[3].name = "ResourceSubType";
    props[3].val = entry->data->ResourceSubType;
    ResourceSettingData.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_CLASSNAME;
    ResourceSettingData.prop_t = props;

    /* build xml params object */
    if (VIR_ALLOC_N(params, 1) < 0)
        goto cleanup;
    params[0].name = "ResourceSettings";
    params[0].type = EMBEDDED_PARAM;
    params[0].param = &ResourceSettingData;

    if (hyperv2InvokeMethod(priv, params, 1, "ModifyResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not add serial device"));
        goto cleanup;
    }
    result = 0;

cleanup:
    hypervFreeObject(priv, (hypervObject *) vssd);
    hypervFreeObject(priv, (hypervObject *) rasd);
    virBufferFreeAndReset(&query);
    VIR_FREE(com_string);
    return result;
}

static int
hyperv2DomainCreateSCSIController(virDomainPtr domain)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    const char *selector =
        "CreationClassname=Msvm_VirtualSystemManagementService";
    virBuffer query = VIR_BUFFER_INITIALIZER;
    properties_t *props = NULL;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    invokeXmlParam *params = NULL;
    embeddedParam ResourceSettingData;
    eprParam vssd_REF;
    char uuid_string[VIR_UUID_STRING_BUFLEN];

    VIR_DEBUG("Attaching SCSI Controller");

    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    virBufferAddLit(&query, MSVM_VIRTUALSYSTEMSETTINGDATA_V2_WQL_SELECT);
    virBufferAsprintf(&query, "where InstanceID=\"%s\"", vssd->data->InstanceID);
    vssd_REF.query = &query;
    vssd_REF.wmiProviderURI = ROOT_VIRTUALIZATION_V2;

    /* build ResourceSettingData param */
    if (VIR_ALLOC_N(props, 3) < 0)
        goto cleanup;
    props[0].name = "ElementName";
    props[0].val = "SCSI Controller";
    props[1].name = "ResourceType";
    props[1].val = "6";
    props[2].name = "ResourceSubType";
    props[2].val = "Microsoft:Hyper-V:Synthetic SCSI Controller";

    ResourceSettingData.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_CLASSNAME;
    ResourceSettingData.prop_t = props;
    ResourceSettingData.nbProps = 3;

    /* build xml params */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "AffectedConfiguration";
    params[0].type = EPR_PARAM;
    params[0].param = &vssd_REF;
    params[1].name = "ResourceSettings";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &ResourceSettingData;

    /* invoke */
    if (hyperv2InvokeMethod(priv, params, 2, "AddResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not attach SCSI controller"));
        goto cleanup;
    }

    result = 0;

cleanup:
    hypervFreeObject(priv, (hypervObject *) vssd);
    VIR_FREE(params);
    VIR_FREE(props);
    virBufferFreeAndReset(&query);
    return result;
}

/* TODO: better error reporting from this function
 * virReportError() doesn't seem to like showing an error occurred
 * this is probably because I don't quite understand how the error-reporting
 * facilities work
 */
static int
hyperv2DomainAttachStorageExtent(virDomainPtr domain, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V2 *controller, const char *hostname)
{
    int result = -1;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;
    char *controller__PATH = NULL;
    char *ctrlrInstanceIdEscaped = NULL;
    char *rasdInstanceIdEscaped = NULL;
    char *addressOnParent = NULL;
    char *settings_instance_id = NULL;
    char *rasd__PATH = NULL;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    eprParam vssd_REF;
    embeddedParam rasd_ResourceSettings;
    embeddedParam sasd_ResourceSettings;
    properties_t *props = NULL;
    invokeXmlParam *params = NULL;
    WsXmlDocH response = NULL;


    virUUIDFormat(domain->uuid, uuid_string);

    VIR_DEBUG("Now attaching disk image '%s' with address %d to bus %d of type %d",
            disk->src->path, disk->info.addr.drive.unit,
            disk->info.addr.drive.controller, disk->bus);

    /*
     * Step 1: Create the Msvm_ResourceAllocationSettingData_V2 object
     * that represents the settings for the virtual hard drive
     */

    /* prepare EPR param */
    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    virBufferAddLit(&query, MSVM_VIRTUALSYSTEMSETTINGDATA_V2_WQL_SELECT);
    virBufferAsprintf(&query, " where InstanceID=\"%s\"", vssd->data->InstanceID);
    vssd_REF.query = &query;
    vssd_REF.wmiProviderURI = ROOT_VIRTUALIZATION_V2;

    /* prepare embedded param */
    addressOnParent = virNumToStr(disk->info.addr.drive.unit);

    ctrlrInstanceIdEscaped = virStringReplace(controller->data->InstanceID,
            "\\", "\\\\");
    if (virAsprintf(&controller__PATH, "\\\\%s\\root\\virtualization\\v2:"
                "Msvm_ResourceAllocationSettingData.InstanceID=\"%s\"",
                hostname, ctrlrInstanceIdEscaped) < 0)
        goto cleanup;

    rasd_ResourceSettings.nbProps = 5;
    if (VIR_ALLOC_N(props, rasd_ResourceSettings.nbProps) < 0)
        goto cleanup;
    props[0].name = "ResourceType";
    props[0].val = "17";
    props[1].name = "ResourceSubType";
    props[1].val = "Microsoft:Hyper-V:Synthetic Disk Drive";
    props[2].name = "ElementName";
    props[2].val = "Hard Drive";
    props[3].name = "AddressOnParent";
    props[3].val = addressOnParent;
    props[4].name = "Parent";
    props[4].val = controller__PATH;
    rasd_ResourceSettings.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_CLASSNAME;
    rasd_ResourceSettings.prop_t = props;

    /* create xml params */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "AffectedConfiguration";
    params[0].type = EPR_PARAM;
    params[0].param = &vssd_REF;
    params[1].name = "ResourceSettings";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &rasd_ResourceSettings;

    if (hyperv2InvokeMethod(priv, params, 2, "AddResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                selector, &response) < 0 || !response) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not attach disk"));
        goto cleanup;
    }

    /* step 2: create the virtual settings object for the disk image */

    /* prepare embedded param 2 */

    /* get rasd instance id from response and create __PATH var */
    settings_instance_id = hyperv2GetInstanceIDFromXMLResponse(response);
    if (!settings_instance_id)
        goto cleanup;

    rasdInstanceIdEscaped = virStringReplace(settings_instance_id, "\\", "\\\\");
    if (virAsprintf(&rasd__PATH, "\\\\%s\\root\\virtualization\\v2:"
                "Msvm_ResourceAllocationSettingData.InstanceID=\"%s\"",
                hostname, rasdInstanceIdEscaped) < 0)
        goto cleanup;

    /* reset vssd_REF param */
    virBufferFreeAndReset(&query);
    virBufferAddLit(&query, MSVM_VIRTUALSYSTEMSETTINGDATA_V2_WQL_SELECT);
    virBufferAsprintf(&query, " where InstanceID=\"%s\"", vssd->data->InstanceID);
    vssd_REF.query = &query;

    /* build embedded param */
    VIR_FREE(params);
    VIR_FREE(props);
    sasd_ResourceSettings.nbProps = 5;
    if (VIR_ALLOC_N(props, sasd_ResourceSettings.nbProps) < 0)
        goto cleanup;
    props[0].name = "ElementName";
    props[0].val = "Hard Disk Image";
    props[1].name = "ResourceType";
    props[1].val = "31";
    props[2].name = "ResourceSubType";
    props[2].val = "Microsoft:Hyper-V:Virtual Hard Disk";
    props[3].name = "HostResource";
    props[3].val = disk->src->path;
    props[4].name = "Parent";
    props[4].val = rasd__PATH;
    sasd_ResourceSettings.instanceName = MSVM_STORAGEALLOCATIONSETTINGDATA_V2_CLASSNAME;
    sasd_ResourceSettings.prop_t = props;

    /* create invokeXmlParam */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "AffectedConfiguration";
    params[0].type = EPR_PARAM;
    params[0].param = &vssd_REF;
    params[1].name = "ResourceSettings";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &sasd_ResourceSettings;

    if (hyperv2InvokeMethod(priv, params, 2, "AddResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not attach disk"));
        goto cleanup;
    }

    result = 0;
cleanup:
    VIR_FREE(props);
    VIR_FREE(params);
    VIR_FREE(controller__PATH);
    VIR_FREE(rasd__PATH);
    VIR_FREE(ctrlrInstanceIdEscaped);
    VIR_FREE(rasdInstanceIdEscaped);
    VIR_FREE(addressOnParent);
    VIR_FREE(settings_instance_id);
    hypervFreeObject(priv, (hypervObject *) vssd);
    if (response)
        ws_xml_destroy_doc(response);
    return result;
}

static int
hyperv2DomainAttachPhysicalDisk(virDomainPtr domain, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V2 *controller, const char *hostname)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    char *hostResource = NULL;
    char *controller__PATH = NULL;
    char *instance_temp = NULL;
    char *diskdef_instance = NULL;
    char *builtPath = NULL;
    char **matches = NULL;
    char *addressOnParent = NULL;
    ssize_t found = 0;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    virBuffer query = VIR_BUFFER_INITIALIZER;
    virBuffer rasdQuery = VIR_BUFFER_INITIALIZER;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    Msvm_ResourceAllocationSettingData_V2 *diskdefault = NULL;
    invokeXmlParam *params = NULL;
    properties_t *props = NULL;
    eprParam vssd_REF;
    embeddedParam embeddedparam;

    if (strstr(disk->src->path, "NODRIVE")) {
        /* Hyper-V doesn't let you define LUNs with no connection */
        VIR_DEBUG("Skipping empty LUN '%s' with address %d on bus %d of type %d",
                disk->src->path, disk->info.addr.drive.unit,
                disk->info.addr.drive.controller, disk->bus);
        goto success;
    }

    virUUIDFormat(domain->uuid, uuid_string);
    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    VIR_DEBUG("Now attaching LUN '%s' with address %d to bus %d of type %d",
            disk->src->path, disk->info.addr.drive.unit,
            disk->info.addr.drive.controller, disk->bus);

    /* prepare HostResource */

    /* get Msvm_diskDrive root device ID */
    virBufferAsprintf(&rasdQuery,
            "SELECT * FROM Msvm_ResourceAllocationSettingData "
            "WHERE ResourceSubType = 'Microsoft:Hyper-V:Physical Disk Drive' "
            "AND InstanceID LIKE '%%Default%%'");

    if (hyperv2GetMsvmResourceAllocationSettingDataList(priv, &rasdQuery,
                &diskdefault) < 0 || diskdefault == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not retrieve default Msvm_DiskDrive object"));
        goto cleanup;
    }

    diskdef_instance = diskdefault->data->InstanceID;
    found = virStringSearch(diskdef_instance,
            "([a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12})",
            1, &matches);

    if (found < 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not get Msvm_DiskDrive default InstanceID"));
        goto cleanup;
    }

    if (virAsprintf(&builtPath, "Microsoft:%s\\\\%s", matches[0],
                disk->src->path) < 0)
        goto cleanup;

    /* TODO: fix this so it can access LUNs on different hosts */
    virBufferAsprintf(&query, "\\\\%s\\root\\virtualization\\v2:"
            "Msvm_DiskDrive.CreationClassName=\"Msvm_DiskDrive\","
            "DeviceID=\"%s\",SystemCreationClassName=\"Msvm_ComputerSystem\","
            "SystemName=\"%s\"",
            hostname, builtPath, hostname);
    hostResource = virBufferContentAndReset(&query);

    /* prepare controller's path */
    instance_temp = virStringReplace(controller->data->InstanceID, "\\", "\\\\");
    if (virAsprintf(&controller__PATH, "\\\\%s\\root\\virtualization\\v2:"
                "Msvm_ResourceAllocationSettingData.InstanceID=\"%s\"",
                hostname, instance_temp) < 0) {
        goto cleanup;
    }

    addressOnParent = virNumToStr(disk->info.addr.drive.unit);

    /* prepare EPR param */
    virBufferAddLit(&query, MSVM_VIRTUALSYSTEMSETTINGDATA_V2_WQL_SELECT);
    virBufferAsprintf(&query, " where InstanceID = \"%s\"", vssd->data->InstanceID);
    vssd_REF.query = &query;
    vssd_REF.wmiProviderURI = ROOT_VIRTUALIZATION_V2;

    /* create embedded param */
    embeddedparam.nbProps = 5;
    if (VIR_ALLOC_N(props, embeddedparam.nbProps) < 0)
        goto cleanup;
    props[0].name = "Parent";
    props[0].val = controller__PATH;
    props[1].name = "AddressOnParent";
    props[1].val = addressOnParent;
    props[2].name = "ResourceType";
    props[2].val = "17";
    props[3].name = "ResourceSubType";
    props[3].val = "Microsoft:Hyper-V:Physical Disk Drive";
    props[4].name = "HostResource";
    props[4].val = hostResource;
    embeddedparam.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_CLASSNAME;
    embeddedparam.prop_t = props;

    /* create xml param */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "AffectedConfiguration";
    params[0].type = EPR_PARAM;
    params[0].param = &vssd_REF;
    params[1].name = "ResourceSettings";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam;

    if (hyperv2InvokeMethod(priv, params, 2, "AddResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not add LUN"));
        goto cleanup;
    }

success:
    result = 0;
cleanup:
    VIR_FREE(params);
    VIR_FREE(props);
    VIR_FREE(controller__PATH);
    VIR_FREE(addressOnParent);
    VIR_FREE(hostResource);
    VIR_FREE(instance_temp);
    virBufferFreeAndReset(&query);
    virBufferFreeAndReset(&rasdQuery);
    VIR_FREE(builtPath);
    virStringListFree(matches);
    hypervFreeObject(priv, (hypervObject *) vssd);
    hypervFreeObject(priv, (hypervObject *) diskdefault);
    return result;
}

static int
hyperv2DomainAttachCDROM(virDomainPtr domain, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V2 *controller, const char *hostname)
{
    int result = -1;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;
    char *controller__PATH = NULL;
    char *ctrlrInstanceIdEscaped = NULL;
    char *rasdInstanceIdEscaped = NULL;
    char *addressOnParent = NULL;
    char *settings_instance_id = NULL;
    char *rasd__PATH = NULL;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    eprParam vssd_REF;
    embeddedParam rasd_ResourceSettings;
    embeddedParam sasd_ResourceSettings;
    properties_t *props = NULL;
    invokeXmlParam *params = NULL;
    WsXmlDocH response = NULL;


    virUUIDFormat(domain->uuid, uuid_string);

    VIR_DEBUG("Now attaching CD/DVD '%s' with address %d to bus %d of type %d",
            disk->src->path, disk->info.addr.drive.unit,
            disk->info.addr.drive.controller, disk->bus);

    /*
     * Step 1: Create the Msvm_ResourceAllocationSettingData_V2 object
     * that represents the settings for the virtual hard drive
     */

    /* prepare EPR param */
    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    virBufferAddLit(&query, MSVM_VIRTUALSYSTEMSETTINGDATA_V2_WQL_SELECT);
    virBufferAsprintf(&query, " where InstanceID=\"%s\"", vssd->data->InstanceID);
    vssd_REF.query = &query;
    vssd_REF.wmiProviderURI = ROOT_VIRTUALIZATION_V2;

    /* prepare embedded param */
    addressOnParent = virNumToStr(disk->info.addr.drive.unit);

    ctrlrInstanceIdEscaped = virStringReplace(controller->data->InstanceID,
            "\\", "\\\\");
    if (virAsprintf(&controller__PATH, "\\\\%s\\root\\virtualization\\v2:"
                "Msvm_ResourceAllocationSettingData.InstanceID=\"%s\"",
                hostname, ctrlrInstanceIdEscaped) < 0)
        goto cleanup;

    rasd_ResourceSettings.nbProps = 5;
    if (VIR_ALLOC_N(props, rasd_ResourceSettings.nbProps) < 0)
        goto cleanup;
    props[0].name = "ResourceType";
    props[0].val = "17";
    props[1].name = "ResourceSubType";
    props[1].val = "Microsoft:Hyper-V:Synthetic DVD Drive";
    props[2].name = "ElementName";
    props[2].val = "Hard Drive";
    props[3].name = "AddressOnParent";
    props[3].val = addressOnParent;
    props[4].name = "Parent";
    props[4].val = controller__PATH;
    rasd_ResourceSettings.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_CLASSNAME;
    rasd_ResourceSettings.prop_t = props;

    /* create xml params */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "AffectedConfiguration";
    params[0].type = EPR_PARAM;
    params[0].param = &vssd_REF;
    params[1].name = "ResourceSettings";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &rasd_ResourceSettings;

    if (hyperv2InvokeMethod(priv, params, 2, "AddResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                selector, &response) < 0 || !response) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not attach disk"));
        goto cleanup;
    }

    /* step 2: create the virtual settings object for the disk image */

    /* prepare embedded param 2 */

    /* get rasd instance id from response and create __PATH var */
    settings_instance_id = hyperv2GetInstanceIDFromXMLResponse(response);
    if (!settings_instance_id)
        goto cleanup;

    rasdInstanceIdEscaped = virStringReplace(settings_instance_id, "\\", "\\\\");
    if (virAsprintf(&rasd__PATH, "\\\\%s\\root\\virtualization\\v2:"
                "Msvm_ResourceAllocationSettingData.InstanceID=\"%s\"",
                hostname, rasdInstanceIdEscaped) < 0)
        goto cleanup;

    /* reset vssd_REF param */
    virBufferFreeAndReset(&query);
    virBufferAddLit(&query, MSVM_VIRTUALSYSTEMSETTINGDATA_V2_WQL_SELECT);
    virBufferAsprintf(&query, " where InstanceID=\"%s\"", vssd->data->InstanceID);
    vssd_REF.query = &query;

    /* build embedded param */
    VIR_FREE(params);
    VIR_FREE(props);
    sasd_ResourceSettings.nbProps = 5;
    if (VIR_ALLOC_N(props, sasd_ResourceSettings.nbProps) < 0)
        goto cleanup;
    props[0].name = "ElementName";
    props[0].val = "Hard Disk Image";
    props[1].name = "ResourceType";
    props[1].val = "31";
    props[2].name = "ResourceSubType";
    props[2].val = "Microsoft:Hyper-V:Virtual CD/DVD Disk";
    props[3].name = "HostResource";
    props[3].val = disk->src->path;
    props[4].name = "Parent";
    props[4].val = rasd__PATH;
    sasd_ResourceSettings.instanceName = MSVM_STORAGEALLOCATIONSETTINGDATA_V2_CLASSNAME;
    sasd_ResourceSettings.prop_t = props;

    /* create invokeXmlParam */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "AffectedConfiguration";
    params[0].type = EPR_PARAM;
    params[0].param = &vssd_REF;
    params[1].name = "ResourceSettings";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &sasd_ResourceSettings;

    if (hyperv2InvokeMethod(priv, params, 2, "AddResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not attach disk"));
        goto cleanup;
    }

    result = 0;
cleanup:
    VIR_FREE(props);
    VIR_FREE(params);
    VIR_FREE(controller__PATH);
    VIR_FREE(rasd__PATH);
    VIR_FREE(ctrlrInstanceIdEscaped);
    VIR_FREE(rasdInstanceIdEscaped);
    VIR_FREE(addressOnParent);
    VIR_FREE(settings_instance_id);
    hypervFreeObject(priv, (hypervObject *) vssd);
    if (response)
        ws_xml_destroy_doc(response);
    return result;
}

static int
hyperv2DomainAttachFloppy(virDomainPtr domain, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V2 *driveSettings, const char *hostname)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    invokeXmlParam *params = NULL;
    properties_t *props = NULL;
    embeddedParam embeddedparam;
    eprParam eprparam;
    char *instance_temp = NULL;
    char *settings__PATH = NULL;

    virUUIDFormat(domain->uuid, uuid_string);
    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    VIR_DEBUG("Attaching floppy image '%s'", disk->src->path);

    /* prepare PATH string */
    instance_temp = virStringReplace(driveSettings->data->InstanceID, "\\", "\\\\");
    if (virAsprintf(&settings__PATH, "\\\\%s\\root\\virtualization:"
                "Msvm_ResourceAllocationSettingData.InstanceID=\"%s\"",
                hostname, instance_temp) < 0)
        goto cleanup;

    /* prepare embedded param */
    embeddedparam.nbProps = 4;
    if (VIR_ALLOC_N(props, embeddedparam.nbProps) < 0)
        goto cleanup;
    props[0].name = "Parent";
    props[0].val = settings__PATH;
    props[1].name = "HostResource";
    props[1].val = disk->src->path;
    props[2].name = "ResourceType";
    props[2].val = "31";
    props[3].name = "ResourceSubType";
    props[3].val = "Microsoft:Hyper-V:Virtual Floppy Disk";
    embeddedparam.instanceName = MSVM_STORAGEALLOCATIONSETTINGDATA_V2_CLASSNAME;
    embeddedparam.prop_t = props;

    virBufferAddLit(&query, MSVM_VIRTUALSYSTEMSETTINGDATA_V2_WQL_SELECT);
    virBufferAsprintf(&query, " where InstanceID = \"%s\"", vssd->data->InstanceID);
    eprparam.query = &query;
    eprparam.wmiProviderURI = ROOT_VIRTUALIZATION_V2;

    /* create xml params */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "AffectedConfiguration";
    params[0].type = EPR_PARAM;
    params[0].param = &eprparam;
    params[1].name = "ResourceSettings";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam;

    if (hyperv2InvokeMethod(priv, params, 2, "AddResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not add floppy disk"));
        goto cleanup;
    }

    result = 0;
cleanup:
    VIR_FREE(params);
    VIR_FREE(props);
    VIR_FREE(instance_temp);
    VIR_FREE(settings__PATH);
    virBufferFreeAndReset(&query);
    hypervFreeObject(priv, (hypervObject *) vssd);
    return result;
}

static int
hyperv2DomainAttachStorageVolume(virDomainPtr domain, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V2 *controller, const char *hostname)
{
    switch (disk->device) {
        case VIR_DOMAIN_DISK_DEVICE_DISK:
            if (disk->src->type == VIR_STORAGE_TYPE_FILE) {
                return hyperv2DomainAttachStorageExtent(domain, disk, controller,
                        hostname);
            } else if (disk->src->type == VIR_STORAGE_TYPE_BLOCK) {
                return hyperv2DomainAttachPhysicalDisk(domain, disk, controller,
                        hostname);
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid disk type"));
                return -1;
            }
        case VIR_DOMAIN_DISK_DEVICE_CDROM:
            return hyperv2DomainAttachCDROM(domain, disk, controller, hostname);
        default:
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid disk bus"));
            return -1;
    }
}

static int
hyperv2DomainAttachStorage(virDomainPtr domain, virDomainDefPtr def,
        const char *hostname)
{
    int result = -1;
    int num_scsi_controllers = 0;
    int i = 0;
    int ctrlr_idx = -1;
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    Msvm_ResourceAllocationSettingData_V2 *rasd = NULL;
    Msvm_ResourceAllocationSettingData_V2 *entry = NULL;
    Msvm_ResourceAllocationSettingData_V2 *ideControllers[HYPERV2_MAX_IDE_CONTROLLERS];
    Msvm_ResourceAllocationSettingData_V2 *scsiControllers[HYPERV2_MAX_SCSI_CONTROLLERS];
    Msvm_ResourceAllocationSettingData_V2 *floppySettings = NULL;

    virUUIDFormat(domain->uuid, uuid_string);

    /* start with attaching scsi controllers */
    for (i = 0; i < def->ncontrollers; i++) {
        if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI) {
            /* we have a scsi controller */
            if (hyperv2DomainCreateSCSIController(domain) < 0)
                goto cleanup;
        }
    }

    /*
     * filter through all the rasd entries and isolate our controllers
     */
    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv2GetRASDByVSSDInstanceId(priv, vssd->data->InstanceID, &rasd) < 0)
        goto cleanup;

    entry = rasd;
    while (entry != NULL) {
        switch (entry->data->ResourceType) {
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_IDE_CONTROLLER:
                ideControllers[entry->data->Address[0] - '0'] = entry;
                break;
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_PARALLEL_SCSI_HBA:
                scsiControllers[num_scsi_controllers++] = entry;
                break;
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_FLOPPY:
                floppySettings = entry;
                break;
        }
        entry = entry->next;
    }

    /* now we loop through and attach all the disks */
    for (i = 0; i < def->ndisks; i++) {
        ctrlr_idx = def->disks[i]->info.addr.drive.controller;

        switch (def->disks[i]->bus) {
            case VIR_DOMAIN_DISK_BUS_IDE:
                /* ide disk */
                if (hyperv2DomainAttachStorageVolume(domain, def->disks[i],
                            ideControllers[ctrlr_idx], hostname) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                            _("Could not attach disk to IDE controller"));
                    goto cleanup;
                }
                break;
            case VIR_DOMAIN_DISK_BUS_SCSI:
                /* scsi disk */
                if (hyperv2DomainAttachStorageVolume(domain, def->disks[i],
                            scsiControllers[ctrlr_idx], hostname) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                            _("Could not attach disk to SCSI controller"));
                    goto cleanup;
                }
                break;
            case VIR_DOMAIN_DISK_BUS_FDC:
                /* floppy disk */
                if (hyperv2DomainAttachFloppy(domain, def->disks[i],
                            floppySettings, hostname) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                            _("Could not attach floppy disk"));
                    goto cleanup;
                }
                break;
            default:
                virReportError(VIR_ERR_INTERNAL_ERROR, _("Unsupported controller type"));
                goto cleanup;
        }
    }

    result = 0;
cleanup:
    hypervFreeObject(priv, (hypervObject *) rasd);
    hypervFreeObject(priv, (hypervObject *) vssd);
    return result;
}

/*
 * Exposed driver API funtions. Everything below here is part of the libvirt
 * driver interface
 */

const char *
hyperv2ConnectGetType(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return "Hyper-V";
}

int
hyperv2ConnectGetVersion(virConnectPtr conn, unsigned long *version)
{
    int result = -1;
    hypervPrivate *priv = conn->privateData;
    Win32_OperatingSystem *os = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    char *p;

    virBufferAddLit(&query, "Select * from Win32_OperatingSystem ");
    if (hypervGetWin32OperatingSystemList(priv, &query, &os) < 0)
        goto cleanup;

    if (os == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not lookup data file for domain %s"),
                "Msvm_VirtualSystemSettingData");
        goto cleanup;
    }

    /*
     * Truncate micro to 3 digits
     */
    p = strrchr(os->data->Version, '.');
    p[4] = '\0';

    /* Parse version string to long */
    if (virParseVersionString(os->data->Version,
                version, true) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not parse version number from '%s'"),
                os->data->Version);
        goto cleanup;
    }

    result = 0;

cleanup:
    hypervFreeObject(priv, (hypervObject *) os);
    virBufferFreeAndReset(&query);
    return result;
}

char *
hyperv2ConnectGetHostname(virConnectPtr conn)
{
    char *hostname = NULL;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Win32_ComputerSystem *computerSystem = NULL;

    virBufferAddLit(&query, WIN32_COMPUTERSYSTEM_WQL_SELECT);

    if (hyperv2GetPhysicalSystemList(priv, &computerSystem) < 0)
        goto cleanup;

    ignore_value(VIR_STRDUP(hostname, computerSystem->data->DNSHostName));

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return hostname;
}

int
hyperv2ConnectGetMaxVcpus(virConnectPtr conn, const char *type ATTRIBUTE_UNUSED)
{
    int result = -1;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ProcessorSettingData_V2 *processorSettingData = NULL;

    /* Get max processors definition */
    virBufferAddLit(&query, "SELECT * FROM Msvm_ProcessorSettingData "
            "WHERE InstanceID LIKE 'Microsoft:Definition%Maximum'");

    if (hyperv2GetMsvmProcessorSettingDataList(priv, &query,
                &processorSettingData) < 0) {
        goto cleanup;
    }

    if (processorSettingData == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not get maximum definition of Msvm_ProcessorSettingData"));
        goto cleanup;
    }

    /* TODO: check if this is still the right number */
    result = processorSettingData->data->VirtualQuantity;

cleanup:
    hypervFreeObject(priv, (hypervObject *) processorSettingData);
    virBufferFreeAndReset(&query);

    return result;
}

int
hyperv2NodeGetInfo(virConnectPtr conn, virNodeInfoPtr info)
{
    int result = -1;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Win32_ComputerSystem *computerSystem = NULL;
    Win32_Processor *processorList = NULL;
    Win32_Processor *processor = NULL;
    char *tmp;

    memset(info, 0, sizeof(*info));

    virBufferAddLit(&query, WIN32_COMPUTERSYSTEM_WQL_SELECT);

    /* Get Win32_ComputerSystem */
    if (hypervGetWin32ComputerSystemList(priv, &query, &computerSystem) < 0)
        goto cleanup;

    if (computerSystem == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s"),
                       "Win32_ComputerSystem");
        goto cleanup;
    }

    if (hyperv2GetProcessorsByName(priv, computerSystem->data->Name,
                &processorList) < 0) {
        goto cleanup;
    }

    /* Strip the string to fit more relevant information in 32 chars */
    tmp = processorList->data->Name;

    while (*tmp != '\0') {
        if (STRPREFIX(tmp, "  ")) {
            memmove(tmp, tmp + 1, strlen(tmp + 1) + 1);
            continue;
        } else if (STRPREFIX(tmp, "(R)") || STRPREFIX(tmp, "(C)")) {
            memmove(tmp, tmp + 3, strlen(tmp + 3) + 1);
            continue;
        } else if (STRPREFIX(tmp, "(TM)")) {
            memmove(tmp, tmp + 4, strlen(tmp + 4) + 1);
            continue;
        }

        ++tmp;
    }

    /* Fill struct */
    if (virStrncpy(info->model, processorList->data->Name,
                   sizeof(info->model) - 1, sizeof(info->model)) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU model %s too long for destination"),
                       processorList->data->Name);
        goto cleanup;
    }

    info->memory = computerSystem->data->TotalPhysicalMemory / 1024; /* byte to kilobyte */
    info->mhz = processorList->data->MaxClockSpeed;
    info->nodes = 1;
    info->sockets = 0;

    for (processor = processorList; processor != NULL;
         processor = processor->next) {
        ++info->sockets;
    }

    info->cores = processorList->data->NumberOfCores;
    info->threads = processorList->data->NumberOfLogicalProcessors / info->cores;
    info->cpus = info->sockets * info->cores;

    result = 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);
    hypervFreeObject(priv, (hypervObject *)processorList);

    return result;
}

int
hyperv2ConnectListDomains(virConnectPtr conn, int *ids, int maxids)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystemList = NULL;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    int count = 0;

    if (maxids == 0)
        return 0;

    if (hyperv2GetActiveVirtualSystemList(priv, &computerSystemList) < 0)
        goto cleanup;

    for (computerSystem = computerSystemList; computerSystem != NULL;
         computerSystem = computerSystem->next) {
        ids[count++] = computerSystem->data->ProcessID;

        if (count >= maxids)
            break;
    }

    success = true;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystemList);

    return success ? count : -1;
}

int
hyperv2ConnectNumOfDomains(virConnectPtr conn)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystemList = NULL;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    int count = 0;

    if (hyperv2GetActiveVirtualSystemList(priv, &computerSystemList) < 0)
        goto cleanup;

    for (computerSystem = computerSystemList; computerSystem != NULL;
         computerSystem = computerSystem->next) {
        ++count;
    }

    success = true;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystemList);

    return success ? count : -1;
}

virDomainPtr
hyperv2DomainCreateXML(virConnectPtr conn, const char *xmlDesc,
        unsigned int flags)
{
    virDomainPtr domain;

    virCheckFlags(VIR_DOMAIN_START_PAUSED | VIR_DOMAIN_START_AUTODESTROY, NULL);

    /* create the new domain */
    domain = hyperv2DomainDefineXML(conn, xmlDesc);
    if (domain == NULL)
        return NULL;

    /* start the domain */
    if (hyperv2InvokeMsvmComputerSystemRequestStateChange(domain,
                MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_ENABLED) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not start domain %s"), domain->name);
        return domain;
    }

    /* If VIR_DOMAIN_START_PAUSED is set, the guest domain will be started, but
     * its CPUs will remain paused */
    if (flags & VIR_DOMAIN_START_PAUSED) {
        /* TODO: use hyperv2DomainSuspend to implement this */
    }

    if (flags & VIR_DOMAIN_START_AUTODESTROY) {
        /* TODO: make auto destroy happen */
    }

    return domain;
}

virDomainPtr
hyperv2DomainLookupByID(virConnectPtr conn, int id)
{
    virDomainPtr domain = NULL;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;

    if (hyperv2GetVirtualSystemByID(priv, id, &computerSystem) < 0) {
        virReportError(VIR_ERR_NO_DOMAIN, _("No domain with ID %d"), id);
        goto cleanup;
    }

    hyperv2MsvmComputerSystemToDomain(conn, computerSystem, &domain);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return domain;
}

virDomainPtr
hyperv2DomainLookupByUUID(virConnectPtr conn, const unsigned char *uuid)
{
    virDomainPtr domain = NULL;
    hypervPrivate *priv = conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    Msvm_ComputerSystem_V2 *computerSystem = NULL;

    virUUIDFormat(uuid, uuid_string);

    if (hyperv2GetVirtualSystemByUUID(priv, uuid_string, &computerSystem) < 0) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("No domain with UUID %s"), uuid_string);
        goto cleanup;
    }

    hyperv2MsvmComputerSystemToDomain(conn, computerSystem, &domain);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return domain;
}

virDomainPtr
hyperv2DomainLookupByName(virConnectPtr conn, const char *name)
{
    virDomainPtr domain = NULL;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;

    if (hyperv2GetVirtualSystemByName(priv, name, &computerSystem) < 0) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not active or is in state transition"));
        goto cleanup;
    }

    hyperv2MsvmComputerSystemToDomain(conn, computerSystem, &domain);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return domain;
}

int
hyperv2DomainSuspend(virDomainPtr domain)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (computerSystem->data->EnabledState !=
        MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_ENABLED) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not active"));
        goto cleanup;
    }

    result = hyperv2InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_PAUSED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv2DomainResume(virDomainPtr domain)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (computerSystem->data->EnabledState !=
        MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_PAUSED) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not paused"));
        goto cleanup;
    }

    result = hyperv2InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_ENABLED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv2DomainShutdown(virDomainPtr domain)
{
    return hyperv2DomainShutdownFlags(domain, 0);
}

int
hyperv2DomainShutdownFlags(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    Msvm_ShutdownComponent_V2 *shutdown = NULL;
    bool in_transition = false;
    char uuid[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;
    simpleParam force, reason;
    invokeXmlParam *params;
    char *selector = NULL;

    virCheckFlags(0, -1);
    virUUIDFormat(domain->uuid, uuid);

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (!hyperv2IsMsvmComputerSystemActive(computerSystem, &in_transition) || in_transition) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                _("Domain is not active or in state transition"));
        goto cleanup;
    }

    virBufferAsprintf(&query,
            "Select * from Msvm_ShutdownComponent "
            "where SystemName = \"%s\"", uuid);

    if (hyperv2GetMsvmShutdownComponentList(priv, &query, &shutdown) < 0 ||
            shutdown == NULL)
        goto cleanup;

    if (virAsprintf(&selector,
            "CreationClassName=\"Msvm_ShutdownComponent\"&"
            "DeviceID=\"%s\"&"
            "SystemCreationClassName=\"Msvm_ComputerSystem\"&"
            "SystemName=\"%s\"", shutdown->data->DeviceID, uuid) < 0)
        goto cleanup;

    force.value = "False";
    reason.value = "Planned shutdown via Libvirt";

    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "Force";
    params[0].type = SIMPLE_PARAM;
    params[0].param = &force;
    params[1].name = "Reason";
    params[1].type = SIMPLE_PARAM;
    params[1].param = &reason;

    if (hyperv2InvokeMethod(priv, params, 2, "InitiateShutdown",
                MSVM_SHUTDOWNCOMPONENT_V2_RESOURCE_URI, selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not shutdown domain"));
        goto cleanup;
    }

    result = 0;

cleanup:
    hypervFreeObject(priv, (hypervObject *) computerSystem);
    hypervFreeObject(priv, (hypervObject *) shutdown);
    VIR_FREE(params);
    VIR_FREE(selector);
    return result;
}

int
hyperv2DomainReboot(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    result = hyperv2InvokeMsvmComputerSystemRequestStateChange(domain,
            MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_REBOOT);

cleanup:
    hypervFreeObject(priv, (hypervObject *) computerSystem);
    return result;
}

int
hyperv2DomainDestroyFlags(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    bool in_transition = false;

    virCheckFlags(0, -1);

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (!hyperv2IsMsvmComputerSystemActive(computerSystem, &in_transition) ||
        in_transition) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not active or is in state transition"));
        goto cleanup;
    }

    result = hyperv2InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_DISABLED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);
    return result;
}

int
hyperv2DomainDestroy(virDomainPtr domain)
{
    return hyperv2DomainDestroyFlags(domain, 0);
}

char *
hyperv2DomainGetOSType(virDomainPtr domain ATTRIBUTE_UNUSED)
{
    char *osType;

    ignore_value(VIR_STRDUP(osType, "hvm"));
    return osType;
}

unsigned long long
hyperv2DomainGetMaxMemory(virDomainPtr domain)
{
    unsigned long long result = 0;
    bool success = false;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    Msvm_MemorySettingData_V2 *mem_sd = NULL;
    hypervPrivate *priv = domain->conn->privateData;

    virUUIDFormat(domain->uuid, uuid_string);

    /* get all the data we need */
    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv2GetMemSDByVSSDInstanceId(priv, vssd->data->InstanceID,
                &mem_sd) < 0)
        goto cleanup;

    result = mem_sd->data->Limit;

    result = result * 1024; /* convert mb to bytes */
    success = true;

cleanup:
    hypervFreeObject(priv, (hypervObject *) vssd);
    hypervFreeObject(priv, (hypervObject *) mem_sd);

    return success ? result : 512; /* default to 512 on failure */
}

int
hyperv2DomainSetMaxMemory(virDomainPtr domain, unsigned long memory)
{
    int result = -1;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    invokeXmlParam *params = NULL;
    hypervPrivate *priv = domain->conn->privateData;
    properties_t *tab_props = NULL;
    embeddedParam embeddedparam;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    Msvm_MemorySettingData_V2 *mem_sd = NULL;
    char *memory_str = NULL;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";

    unsigned long memory_mb = memory / 1024;

    /* memory has to be multiple of 2 mb; round up if necessary */
    if (memory_mb % 2) memory_mb++;

    if (!(memory_str = virNumToStr(memory_mb)))
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    /* get all the data we need */
    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv2GetMemSDByVSSDInstanceId(priv, vssd->data->InstanceID,
                &mem_sd) < 0)
        goto cleanup;

    /* prepare EMBEDDED param */
    embeddedparam.nbProps = 2;
    if (VIR_ALLOC_N(tab_props, embeddedparam.nbProps) < 0)
        goto cleanup;
    tab_props[0].name = "Limit";
    tab_props[0].val = memory_str;
    tab_props[1].name = "InstanceID";
    tab_props[1].val = mem_sd->data->InstanceID;
    embeddedparam.instanceName = MSVM_MEMORYSETTINGDATA_V2_CLASSNAME;
    embeddedparam.prop_t = tab_props;

    /* set up invokeXmlParam */
    if (VIR_ALLOC_N(params, 1) < 0)
        goto cleanup;
    params[0].name = "ResourceSettings";
    params[0].type = EMBEDDED_PARAM;
    params[0].param = &embeddedparam;

    result = hyperv2InvokeMethod(priv, params, 1, "ModifyResourceSettings",
        MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI, selector, NULL);

cleanup:
    VIR_FREE(tab_props);
    VIR_FREE(params);
    VIR_FREE(memory_str);
    hypervFreeObject(priv, (hypervObject *) vssd);
    hypervFreeObject(priv, (hypervObject *) mem_sd);
    return result;
}

int
hyperv2DomainSetMemory(virDomainPtr domain, unsigned long memory)
{
    return hyperv2DomainSetMemoryFlags(domain, memory, 0);
}

int
hyperv2DomainSetMemoryFlags(virDomainPtr domain, unsigned long memory,
        unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    invokeXmlParam *params = NULL;
    properties_t *tab_props = NULL;
    embeddedParam embeddedparam;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    Msvm_MemorySettingData_V2 *mem_sd = NULL;
    char *memory_str = NULL;

    /* memory has to passed as a multiple of 2mb */
    unsigned long memory_mb = memory / 1024;
    if (memory_mb % 2) memory_mb++;

    if (!(memory_str = virNumToStr(memory_mb)))
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    /* get all the data we need */
    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv2GetMemSDByVSSDInstanceId(priv, vssd->data->InstanceID,
                &mem_sd) < 0)
        goto cleanup;

    /* prepare EMBEDDED param */
    embeddedparam.nbProps = 2;
    if (VIR_ALLOC_N(tab_props, embeddedparam.nbProps) < 0)
        goto cleanup;
    tab_props[0].name = "VirtualQuantity";
    tab_props[0].val = memory_str;
    tab_props[1].name = "InstanceID";
    tab_props[1].val = mem_sd->data->InstanceID;
    embeddedparam.instanceName = MSVM_MEMORYSETTINGDATA_V2_CLASSNAME;
    embeddedparam.prop_t = tab_props;

    /* set up invokeXmlParam */
    if (VIR_ALLOC_N(params, 1) < 0)
        goto cleanup;
    params[0].name = "ResourceSettings";
    params[0].type = EMBEDDED_PARAM;
    params[0].param = &embeddedparam;

    if (hyperv2InvokeMethod(priv, params, 1, "ModifyResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not set domain memory"));
        goto cleanup;
    }

    result = 0;

 cleanup:
    VIR_FREE(tab_props);
    VIR_FREE(params);
    VIR_FREE(memory_str);
    hypervFreeObject(priv, (hypervObject *) vssd);
    hypervFreeObject(priv, (hypervObject *) mem_sd);
    return result;
}

int
hyperv2DomainGetInfo(virDomainPtr domain, virDomainInfoPtr info)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    Msvm_VirtualSystemSettingData_V2 *virtualSystemSettingData = NULL;
    Msvm_ProcessorSettingData_V2 *processorSettingData = NULL;
    Msvm_MemorySettingData_V2 *memorySettingData = NULL;

    memset(info, 0, sizeof(*info));

    virUUIDFormat(domain->uuid, uuid_string);

    /* Get Msvm_ComputerSystem_V2 */
    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (hyperv2GetVSSDFromUUID(priv, uuid_string,
                &virtualSystemSettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_VirtualSystemSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    if (hyperv2GetProcSDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &processorSettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_ProcessorSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    if (hyperv2GetMemSDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &memorySettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_MemorySettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Fill struct */
    info->state = hyperv2MsvmComputerSystemEnabledStateToDomainState(computerSystem);
    info->maxMem = memorySettingData->data->Limit * 1024; /* megabyte to kilobyte */
    info->memory = memorySettingData->data->VirtualQuantity * 1024; /* megabyte to kilobyte */
    info->nrVirtCpu = processorSettingData->data->VirtualQuantity;
    info->cpuTime = 0;

    result = 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);
    hypervFreeObject(priv, (hypervObject *)virtualSystemSettingData);
    hypervFreeObject(priv, (hypervObject *)processorSettingData);
    hypervFreeObject(priv, (hypervObject *)memorySettingData);

    return result;
}

int
hyperv2DomainGetState(virDomainPtr domain, int *state, int *reason,
                     unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    *state = hyperv2MsvmComputerSystemEnabledStateToDomainState(computerSystem);

    if (reason != NULL)
        *reason = 0;

    result = 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

static int
hyperv2GetVideoResolution(hypervPrivate *priv, char *vm_uuid, int *xRes,
        int *yRes, bool fallback)
{
    int ret = -1;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VideoHead_V2 *heads = NULL;
    Msvm_S3DisplayController_V2 *s3Display = NULL;
    Msvm_SyntheticDisplayController_V2 *synthetic = NULL;
    const char *wmiClass = "Msvm_SyntheticDisplayController";
    char *deviceId = NULL;

    if (fallback) {
        wmiClass = "Msvm_S3DisplayController";
    }

    virBufferAsprintf(&query, "Select * from %s where SystemName = \"%s\"",
                      wmiClass,
                      vm_uuid);

    if (fallback) {
        if (hyperv2GetMsvmS3DisplayControllerList(priv, &query, &s3Display) < 0 ||
                s3Display == NULL)
            goto cleanup;

        deviceId = s3Display->data->DeviceID;
    } else {
        if (hyperv2GetMsvmSyntheticDisplayControllerList(priv, &query, &synthetic) < 0 ||
                synthetic == NULL)
            goto cleanup;

        deviceId = synthetic->data->DeviceID;
    }

    virBufferFreeAndReset(&query);

    virBufferAsprintf(&query,
            "associators of "
            "{%s."
            "CreationClassName=\"%s\","
            "DeviceID=\"%s\","
            "SystemCreationClassName=\"Msvm_ComputerSystem\","
            "SystemName=\"%s\"} "
            "where AssocClass = Msvm_VideoHeadOnController "
            "ResultClass = Msvm_VideoHead",
            wmiClass, wmiClass, deviceId, vm_uuid);

    if (hyperv2GetMsvmVideoHeadList(priv, &query, &heads) < 0)
        goto cleanup;

    // yep, EnabledState is a "numeric string"...
    if (heads != NULL && STREQLEN(heads->data->EnabledState, "2", 1)) {
        *xRes = heads->data->CurrentHorizontalResolution;
        *yRes = heads->data->CurrentVerticalResolution;
        ret = 0;
    }

 cleanup:
    virBufferFreeAndReset(&query);
    hypervFreeObject(priv, (hypervObject *) s3Display);
    hypervFreeObject(priv, (hypervObject *) synthetic);
    hypervFreeObject(priv, (hypervObject *) heads);

    return ret;
}

char *
hyperv2DomainScreenshot(virDomainPtr domain, virStreamPtr stream,
        unsigned int screen ATTRIBUTE_UNUSED, unsigned int flags ATTRIBUTE_UNUSED)
{
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    eprParam eprparam;
    simpleParam x_param, y_param;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    invokeXmlParam *params;
    WsXmlDocH ret_doc = NULL;
    int xRes = 640;
    int yRes = 480;
    char *imageDataText = NULL;
    char *imageDataBuffer = NULL;
    size_t imageDataBufferSize;
    char thumbnailFilename[VIR_UUID_STRING_BUFLEN + 26];
    uint16_t *bufAs16 = NULL;
    uint16_t px;
    uint8_t *ppmBuffer = NULL;
    char *result = NULL;
    FILE *fd;
    int pixelCount, i = 0;
    const char *xpath =
        "/s:Envelope/s:Body/p:GetVirtualSystemThumbnailImage_OUTPUT/p:ImageData";

    virUUIDFormat(domain->uuid, uuid_string);

    /* in gen1 VMs, there are 2 video heads used, initially S3DisplayConttroller
     * and when guests OS initializes it's video acceleration driver it will
     * switch to SyntheticDisplayController, therefore try to get res from the
     * "synthetic" first then fall back to "s3" */
    if (hyperv2GetVideoResolution(priv, uuid_string, &xRes, &yRes, false) < 0) {
        if (hyperv2GetVideoResolution(priv, uuid_string, &xRes, &yRes, true) < 0)
            goto cleanup;
    }

    /* Prepare EPR param - get Msvm_VirtualSystemSettingData_V2 */
    virBufferFreeAndReset(&query);
    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_ComputerSystem.CreationClassName=\"Msvm_ComputerSystem\","
            "Name=\"%s\"} "
            "where AssocClass = Msvm_SettingsDefineState "
            "ResultClass = Msvm_VirtualSystemSettingData",
            uuid_string);

    eprparam.query = &query;
    eprparam.wmiProviderURI = ROOT_VIRTUALIZATION_V2;

    /* create invokeXmlParam */
    if (VIR_ALLOC_N(params, 3) < 0)
        goto cleanup;

    x_param.value = virNumToStr(xRes);
    y_param.value = virNumToStr(yRes);

    params[0].name = "HeightPixels";
    params[0].type = SIMPLE_PARAM;
    params[0].param = &y_param;
    params[1].name = "WidthPixels";
    params[1].type = SIMPLE_PARAM;
    params[1].param = &x_param;
    params[2].name = "TargetSystem";
    params[2].type = EPR_PARAM;
    params[2].param = &eprparam;

    if (hyperv2InvokeMethod(priv, params, 3, "GetVirtualSystemThumbnailImage",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI, selector,
                &ret_doc) < 0)
        goto cleanup;

    if (!ws_xml_get_soap_envelope(ret_doc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not retrieve thumbnail image"));
        goto cleanup;
    }

    imageDataText = ws_xml_get_xpath_value(ret_doc, (char *) xpath);

    if (!imageDataText) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Failed to retrieve image data"));
        goto cleanup;
    }

    if (!base64_decode_alloc(imageDataText, strlen(imageDataText),
                &imageDataBuffer, &imageDataBufferSize)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Failed to decode image"));
        goto cleanup;
    }

    pixelCount = imageDataBufferSize / 2;
    if (VIR_ALLOC_N(ppmBuffer, pixelCount * 3) < 0)
        goto cleanup;

    /* convert rgb565 to rgb888 */
    bufAs16 = (uint16_t *) imageDataBuffer;
    for (i = 0; i < pixelCount; i++) {
        px = bufAs16[i];
        ppmBuffer[i*3] = ((((px >> 11) & 0x1F) * 527) + 23) >> 6;
        ppmBuffer[i*3+1] = ((((px >> 5) & 0x3F) * 259) + 33) >> 6;
        ppmBuffer[i*3+2] = (((px & 0x1F) * 527) + 23) >> 6;
    }

    sprintf(thumbnailFilename, "/tmp/hyperv_thumb_%s.rgb888", uuid_string);
    if ((fd = fopen(thumbnailFilename, "w")) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not open temp file for writing"));
        goto cleanup;
    }

    /* write image header */
    fprintf(fd, "P6\n%d %d\n255\n", xRes, yRes);

    /* write image data */
    fwrite(ppmBuffer, 3, pixelCount, fd);
    fclose(fd);

    if (virFDStreamOpenFile(stream, (const char *) &thumbnailFilename, 0, 0,
                O_RDONLY) < 0) {
        goto cleanup;
    }

    if (VIR_ALLOC(result) < 0)
        goto cleanup;

    if (VIR_STRDUP(result, "image/x-portable-pixmap") < 0) {
        VIR_FREE(result);
        goto cleanup;
    }

cleanup:
    virBufferFreeAndReset(&query);
    if (ret_doc)
        ws_xml_destroy_doc(ret_doc);
    VIR_FREE(imageDataBuffer);
    VIR_FREE(ppmBuffer);
    VIR_FREE(params);
    return result;
}

int
hyperv2DomainSetVcpus(virDomainPtr domain, unsigned int nvcpus)
{
    return hyperv2DomainSetVcpusFlags(domain, nvcpus, 0);
}

int
hyperv2DomainSetVcpusFlags(virDomainPtr domain, unsigned int nvcpus,
        unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    Msvm_ProcessorSettingData_V2 *proc_sd = NULL;
    hypervPrivate *priv = domain->conn->privateData;
    embeddedParam embeddedparam;
    properties_t *tab_props;
    invokeXmlParam *params = NULL;
    char *nvcpus_str = NULL;

    /* Convert nvcpus into a string value */
    nvcpus_str = virNumToStr(nvcpus);
    if (!nvcpus_str)
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv2GetProcSDByVSSDInstanceId(priv, vssd->data->InstanceID,
                &proc_sd) < 0) {
        goto cleanup;
    }

    embeddedparam.nbProps = 2;
    if (VIR_ALLOC_N(tab_props, embeddedparam.nbProps) < 0)
        goto cleanup;
    tab_props[0].name = "VirtualQuantity";
    tab_props[0].val = nvcpus_str;
    tab_props[1].name = "InstanceID";
    tab_props[1].val = proc_sd->data->InstanceID;
    embeddedparam.instanceName = MSVM_PROCESSORSETTINGDATA_V2_CLASSNAME;
    embeddedparam.prop_t = tab_props;

    /* prepare and invoke method */
    if (VIR_ALLOC_N(params, 1) < 0)
        goto cleanup;
    params[0].name = "ResourceSettings";
    params[0].type = EMBEDDED_PARAM;
    params[0].param = &embeddedparam;

    if (hyperv2InvokeMethod(priv, params, 1, "ModifyResourceSettings",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI, selector, NULL) < 0)
        goto cleanup;

    result = 0;

cleanup:
    VIR_FREE(tab_props);
    VIR_FREE(params);
    VIR_FREE(nvcpus_str);
    hypervFreeObject(priv, (hypervObject *) vssd);
    hypervFreeObject(priv, (hypervObject *) proc_sd);
    return result;
}

int
hyperv2DomainGetVcpusFlags(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    Msvm_ProcessorSettingData_V2 *proc_sd = NULL;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;

    virCheckFlags(VIR_DOMAIN_VCPU_LIVE |
                  VIR_DOMAIN_VCPU_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM, -1);


    virUUIDFormat(domain->uuid, uuid_string);

    /* Start by getting the Msvm_ComputerSystem_V2 */
    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    /* Check @flags to see if we are to query a running domain, and fail
     * if that domain is not running */
    if (flags & VIR_DOMAIN_VCPU_LIVE) {
        if (computerSystem->data->EnabledState != MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_ENABLED) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s", _("Domain is not active"));
            goto cleanup;
        }
    }

    /* Check @flags to see if we are to return the maximum vCPU limit */
    if (flags & VIR_DOMAIN_VCPU_MAXIMUM) {
        result = hyperv2ConnectGetMaxVcpus(domain->conn, NULL);
        goto cleanup;
    }

    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv2GetProcSDByVSSDInstanceId(priv, vssd->data->InstanceID,
                &proc_sd) < 0) {
        goto cleanup;
    }

    result = proc_sd->data->VirtualQuantity;

cleanup:
    hypervFreeObject(priv, (hypervObject *) computerSystem);
    hypervFreeObject(priv, (hypervObject *) vssd);
    hypervFreeObject(priv, (hypervObject *) proc_sd);
    virBufferFreeAndReset(&query);
    return result;
}

int
hyperv2DomainGetVcpus(virDomainPtr domain, virVcpuInfoPtr info, int maxinfo,
        unsigned char *cpumaps, int maplen)
{
    int count = 0, i;
    hypervPrivate *priv = domain->conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Win32_PerfRawData_HvStats_HyperVHypervisorVirtualProcessor
        *vproc = NULL;

    /* No cpumaps info returned by this api, so null out cpumaps */
    if ((cpumaps != NULL) && (maplen > 0))
        memset(cpumaps, 0, maxinfo * maplen);

    for (i = 0; i < maxinfo; i++) {
        /* try to free objects from previous iteration */
        hypervFreeObject(priv, (hypervObject *) vproc);
        vproc = NULL;
        virBufferFreeAndReset(&query);
        virBufferAddLit(&query, WIN32_PERFRAWDATA_HVSTATS_HYPERVHYPERVISORVIRTUALPROCESSOR_WQL_SELECT);
        /* Attribute Name format : <domain_name>:Hv VP <vCPU_number> */
        virBufferAsprintf(&query, "where Name = \"%s:Hv VP %d\"",
                domain->name, i);

        /* get the info */
        if (hypervGetWin32PerfRawDataHvStatsHyperVHypervisorVirtualProcessorList(
                    priv, &query, &vproc) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Could not get stats on vCPU #%d"), i);
            continue;
        }

        /* Fill structure info */
        info[i].number = i;
        if (vproc) {
            info[i].state = VIR_VCPU_RUNNING;
            info[i].cpuTime = vproc->data->PercentTotalRunTime;
            info[i].cpu = i;
        } else {
            info[i].state = VIR_VCPU_OFFLINE;
            info[i].cpuTime = 0LLU;
            info[i].cpu = -1;
        }
        count++;
    }

    hypervFreeObject(priv, (hypervObject *) vproc);
    virBufferFreeAndReset(&query);

    return count;
}

int
hyperv2DomainGetMaxVcpus(virDomainPtr dom)
{
    /* If the guest is inactive, this is basically the same as virConnectGetMaxVcpus() */
    return (hyperv2DomainIsActive(dom)) ?
        hyperv2DomainGetVcpusFlags(dom, (VIR_DOMAIN_VCPU_LIVE | VIR_DOMAIN_VCPU_MAXIMUM))
        : hyperv2ConnectGetMaxVcpus(dom->conn, NULL);
}

char *
hyperv2DomainGetXMLDesc(virDomainPtr domain, unsigned int flags)
{
    char *xml = NULL;
    hypervPrivate *priv = domain->conn->privateData;
    virDomainDefPtr def = NULL;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    char **notes = NULL;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    Msvm_VirtualSystemSettingData_V2 *virtualSystemSettingData = NULL;
    Msvm_ProcessorSettingData_V2 *processorSettingData = NULL;
    Msvm_MemorySettingData_V2 *memorySettingData = NULL;
    Msvm_ResourceAllocationSettingData_V2 *rasd = NULL;
    Msvm_EthernetPortAllocationSettingData_V2 *nets = NULL;
    Msvm_StorageAllocationSettingData_V2 *sasd = NULL;

    /* Flags checked by virDomainDefFormat */

    if (!(def = virDomainDefNew()))
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    /* Get Msvm_ComputerSystem_V2 */
    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (hyperv2GetVSSDFromUUID(priv, uuid_string,
                &virtualSystemSettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_VirtualSystemSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_ProcessorSettingData_V2 */
    if (hyperv2GetProcSDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &processorSettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_ProcessorSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_MemorySettingData_V2 */
    if (hyperv2GetMemSDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &memorySettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_MemorySettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_ResourceAllocationSettingData_V2 */
    if (hyperv2GetRASDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &rasd) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not get resource information for domain %s"),
                computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_StorageAllocationSettingData_V2 */
    if (hyperv2GetSASDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &sasd) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not get storage information for domain %s"),
                computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_SyntheticEthernetPortSettingData_V2 */
    if (hyperv2GetEthernetPortAllocationSDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &nets) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not get ethernet adapters for domain %s"),
                computerSystem->data->ElementName);
    }

    /* Fill struct */
    def->virtType = VIR_DOMAIN_VIRT_HYPERV;

    if (hyperv2IsMsvmComputerSystemActive(computerSystem, NULL)) {
        def->id = computerSystem->data->ProcessID;
    } else {
        def->id = -1;
    }

    if (virUUIDParse(computerSystem->data->Name, def->uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not parse UUID from string '%s'"),
                       computerSystem->data->Name);
        return NULL;
    }

    if (VIR_STRDUP(def->name, computerSystem->data->ElementName) < 0)
        goto cleanup;

    /* TODO: check if this gets all the note content */
    if (virtualSystemSettingData->data->Notes.count > 0) {
        notes = virtualSystemSettingData->data->Notes.data;
        if (VIR_STRDUP(def->description, *notes) < 0)
            goto cleanup;
    }

    virDomainDefSetMemoryTotal(def, memorySettingData->data->Limit * 1024); /* megabyte to kilobyte */
    def->mem.cur_balloon = memorySettingData->data->VirtualQuantity * 1024; /* megabyte to kilobyte */

    if (virDomainDefSetVcpusMax(def,
                                processorSettingData->data->VirtualQuantity,
                                NULL) < 0)
        goto cleanup;

    if (virDomainDefSetVcpus(def,
                             processorSettingData->data->VirtualQuantity) < 0)
        goto cleanup;

    def->os.type = VIR_DOMAIN_OSTYPE_HVM;

    /* Allocate space for all potential devices */
    if (VIR_ALLOC_N(def->disks, 264) < 0) { /* 256 scsi drives + 8 ide drives */
        goto cleanup;
    }

    if (VIR_ALLOC_N(def->controllers, 6) < 0) { /* 2 ide + 4 scsi */
        goto cleanup;
    }

    if (VIR_ALLOC_N(def->nets, 12) < 0) { /* 8 synthetic + 4 legacy */
        goto cleanup;
    }

    def->ndisks = 0;
    def->ncontrollers = 0;
    def->nnets = 0;
    def->nserials = 0;

    /* FIXME: devices section is totally missing */
    if (hyperv2DomainDefParseStorage(domain, def, rasd, sasd) < 0)
        goto cleanup;

    if (hyperv2DomainDefParseSerial(domain, def, rasd) < 0)
        goto cleanup;

    if (hyperv2DomainDefParseEthernet(domain, def, nets) < 0)
        goto cleanup;

    xml = virDomainDefFormat(def, NULL,
                             virDomainDefFormatConvertXMLFlags(flags));

 cleanup:
    virDomainDefFree(def);
    hypervFreeObject(priv, (hypervObject *)computerSystem);
    hypervFreeObject(priv, (hypervObject *)virtualSystemSettingData);
    hypervFreeObject(priv, (hypervObject *)processorSettingData);
    hypervFreeObject(priv, (hypervObject *)memorySettingData);
    hypervFreeObject(priv, (hypervObject *)rasd);
    hypervFreeObject(priv, (hypervObject *)nets);

    return xml;
}

int
hyperv2ConnectListDefinedDomains(virConnectPtr conn, char **const names,
        int maxnames)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystemList = NULL;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    int count = 0;
    size_t i;

    if (maxnames == 0)
        return 0;

    if (hyperv2GetInactiveVirtualSystemList(priv, &computerSystemList) < 0)
        goto cleanup;

    for (computerSystem = computerSystemList; computerSystem != NULL;
         computerSystem = computerSystem->next) {
        if (VIR_STRDUP(names[count], computerSystem->data->ElementName) < 0)
            goto cleanup;

        ++count;

        if (count >= maxnames)
            break;
    }

    success = true;

 cleanup:
    if (!success) {
        for (i = 0; i < count; ++i)
            VIR_FREE(names[i]);

        count = -1;
    }

    hypervFreeObject(priv, (hypervObject *)computerSystemList);

    return count;
}

int
hyperv2ConnectNumOfDefinedDomains(virConnectPtr conn)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystemList = NULL;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    int count = 0;

    if (hyperv2GetInactiveVirtualSystemList(priv, &computerSystemList) < 0)
        goto cleanup;

    for (computerSystem = computerSystemList; computerSystem != NULL;
         computerSystem = computerSystem->next) {
        ++count;
    }

    success = true;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystemList);

    return success ? count : -1;
}

int
hyperv2DomainCreateWithFlags(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (hyperv2IsMsvmComputerSystemActive(computerSystem, NULL)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is already active or is in state transition"));
        goto cleanup;
    }

    result = hyperv2InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_ENABLED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv2DomainCreate(virDomainPtr domain)
{
    return hyperv2DomainCreateWithFlags(domain, 0);
}

virDomainPtr
hyperv2DomainDefineXML(virConnectPtr conn, const char *xml)
{
    hypervPrivate *priv = conn->privateData;
    virDomainDefPtr def = NULL;
    virDomainPtr domain = NULL;
    invokeXmlParam *params = NULL;
    properties_t *tab_props = NULL;
    embeddedParam embedded_param;
    Msvm_ComputerSystem_V2 *host = NULL;
    int i = 0;
    int nb_params;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    char *hostname = NULL;
    bool success = false;

    if (hyperv2GetHostSystem(priv, &host) < 0)
        goto cleanup;

    hostname = host->data->ElementName;

    /* parse xml */
    def = virDomainDefParseString(xml, priv->caps, priv->xmlopt, NULL,
            1 << VIR_DOMAIN_VIRT_HYPERV | VIR_DOMAIN_XML_INACTIVE);

    if (def == NULL)
        goto cleanup;

    /* create the domain if it doesn't exist already */
    if (def->uuid == NULL ||
            (domain = hyperv2DomainLookupByUUID(conn, def->uuid)) == NULL) {
        /* Prepare params. edit only vm name for now */
        embedded_param.nbProps = 1;
        if (VIR_ALLOC_N(tab_props, embedded_param.nbProps) < 0)
            goto cleanup;

        tab_props[0].name = "ElementName";
        tab_props[0].val = def->name;
        embedded_param.instanceName = MSVM_VIRTUALSYSTEMSETTINGDATA_V2_CLASSNAME;
        embedded_param.prop_t = tab_props;

        /* Create XML params for method invocation */
        nb_params = 1;
        if (VIR_ALLOC_N(params, nb_params) < 0)
            goto cleanup;
        params[0].name = "SystemSettings";
        params[0].type = EMBEDDED_PARAM;
        params[0].param = &embedded_param;

        /* Actually invoke the method to create the VM */
        if (hyperv2InvokeMethod(priv, params, nb_params, "DefineSystem",
                    MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI,
                    selector, NULL) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Could not create new domain %s"), def->name);
            goto cleanup;
        }

        /* populate a domain ptr so that we can edit it */
        domain = hyperv2DomainLookupByName(conn, def->name);
    }

    /* set domain vcpus */
    if (def->vcpus) {
        if (hyperv2DomainSetVcpus(domain, def->maxvcpus) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("Could not set VM vCPUs"));
            goto cleanup;
        }
    }

    /* Set VM maximum memory */
    if (def->mem.max_memory > 0) {
        if (hyperv2DomainSetMaxMemory(domain, def->mem.max_memory) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not set VM maximum memory"));
            goto cleanup;
        }
    }

    /* Set VM memory */
    if (def->mem.cur_balloon > 0) {
        if (hyperv2DomainSetMemory(domain, def->mem.cur_balloon) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not set VM memory"));
            goto cleanup;
        }
    }

    /* Attach networks */
    for (i = 0; i < def->nnets; i++) {
        if (hyperv2DomainAttachSyntheticEthernetAdapter(domain, def->nets[i],
                    hostname) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Could not attach network"));
            goto cleanup;
        }
    }

    /* Attach serials */
    for (i = 0; i < def->nserials; i++) {
        if (hyperv2DomainAttachSerial(domain, def->serials[i]) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not attach serial"));
            goto cleanup;
        }
    }

    /* Attach all storage */
    if (hyperv2DomainAttachStorage(domain, def, hostname) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not attach storage"));
        goto cleanup;
    }

    success = true;
    VIR_DEBUG("Domain created! name: %s, uuid: %s",
            domain->name, virUUIDFormat(domain->uuid, uuid_string));

cleanup:
    virDomainDefFree(def);
    VIR_FREE(tab_props);
    VIR_FREE(params);
    if (success) {
        return domain;
    } else {
        VIR_DEBUG("Domain creation failed, rolling back");
        if (domain)
            ignore_value(hyperv2DomainUndefine(domain));
        return NULL;
    }
}

int
hyperv2DomainUndefine(virDomainPtr domain)
{
    return hyperv2DomainUndefineFlags(domain, 0);
}

int
hyperv2DomainUndefineFlags(virDomainPtr domain, unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    invokeXmlParam *params = NULL;
    eprParam eprparam;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;

    virCheckFlags(0, -1);
    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    /* try to shut down the VM if it's not disabled, just to be safe */
    if (computerSystem->data->EnabledState != MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_DISABLED) {
        if (hyperv2DomainShutdown(domain) < 0)
            goto cleanup;
    }

    /* prepare params */
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_SELECT);
    virBufferAsprintf(&query, "where Name = \"%s\"", uuid_string);
    eprparam.query = &query;
    eprparam.wmiProviderURI = ROOT_VIRTUALIZATION_V2;

    if (VIR_ALLOC_N(params, 1) < 0)
        goto cleanup;
    params[0].name = "AffectedSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &eprparam;

    /* actually destroy the vm */
    if (hyperv2InvokeMethod(priv, params, 1, "DestroySystem",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI, selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not delete domain"));
        goto cleanup;
    }

    result = 0;

cleanup:
    VIR_FREE(params);
    hypervFreeObject(priv, (hypervObject *) computerSystem);
    virBufferFreeAndReset(&query);
    return result;
}

int
hyperv2DomainAttachDevice(virDomainPtr domain, const char *xml)
{
    return hyperv2DomainAttachDeviceFlags(domain, xml, 0);
}

int
hyperv2DomainAttachDeviceFlags(virDomainPtr domain, const char *xml,
        unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    char *xmlDomain = NULL;
    virDomainDefPtr def = NULL;
    virDomainDeviceDefPtr dev = NULL;
    Msvm_ComputerSystem_V2 *host = NULL;
    char *hostname = NULL;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    Msvm_ResourceAllocationSettingData_V2 *controller = NULL;
    Msvm_ResourceAllocationSettingData_V2 *rasd = NULL, *entry = NULL;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    int num_scsi = 0;

    virUUIDFormat(domain->uuid, uuid_string);

    /* get domain definition */
    if ((xmlDomain = hyperv2DomainGetXMLDesc(domain, 0)) == NULL)
        goto cleanup;

    if ((def = virDomainDefParseString(xmlDomain, priv->caps, priv->xmlopt,
                    NULL, 1 << VIR_DOMAIN_VIRT_HYPERV | VIR_DOMAIN_XML_INACTIVE)) == NULL)
        goto cleanup;

    /* get domain device definition */
    if ((dev = virDomainDeviceDefParse(xml, def, priv->caps, priv->xmlopt,
                    VIR_DOMAIN_XML_INACTIVE)) == NULL)
        goto cleanup;

    /* get the host computer system */
    if (hyperv2GetHostSystem(priv, &host) < 0)
        goto cleanup;

    hostname = host->data->ElementName;

    switch (dev->type) {
        case VIR_DOMAIN_DEVICE_DISK:
            /* get our controller
             *
             * TODO: if it turns out that the order is not the same across
             * invocations, implement saving DeviceID of SCSI controllers
             */
            if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
                goto cleanup;

            if (hyperv2GetRASDByVSSDInstanceId(priv, vssd->data->InstanceID, &rasd) < 0)
                goto cleanup;

            entry = rasd;
            /*
             * The logic here is adapted from the controller identification loop
             * in hyperv2DomainAttachStorage(). This code tries to perform in the
             * same way to make things as consistent as possible.
             */
            switch (dev->data.disk->bus) {
                case VIR_DOMAIN_DISK_BUS_IDE:
                    while (entry != NULL) {
                        if (entry->data->ResourceType ==
                                MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_IDE_CONTROLLER) {
                            if ((entry->data->Address[0] - '0') ==
                                    dev->data.disk->info.addr.drive.controller) {
                                controller = entry;
                                break;
                            }
                        }
                        entry = entry->next;
                    }
                    if (entry == NULL)
                        goto cleanup;
                    break;
                case VIR_DOMAIN_DISK_BUS_SCSI:
                    while (entry != NULL) {
                        if (entry->data->ResourceType ==
                                MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_PARALLEL_SCSI_HBA) {
                            if (num_scsi++ ==
                                    dev->data.disk->info.addr.drive.controller) {
                                controller = entry;
                                break;
                            }
                        }
                        entry = entry->next;
                    }
                    if (entry == NULL)
                        goto cleanup;
                    break;
                case VIR_DOMAIN_DISK_BUS_FDC:
                    while (entry != NULL) {
                        if (entry->data->ResourceType ==
                                MSVM_RESOURCEALLOCATIONSETTINGDATA_V2_RESOURCETYPE_FLOPPY) {
                            controller = entry;
                            break;
                        }
                        entry = entry->next;
                    }
                    if (entry == NULL)
                        goto cleanup;
                    break;
                default:
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                            _("Invalid disk bus in definition"));
                    goto cleanup;
            }

            if (hyperv2DomainAttachStorageVolume(domain, dev->data.disk,
                        controller, hostname) < 0)
                goto cleanup;
            break;
        case VIR_DOMAIN_DEVICE_NET:
            if (hyperv2DomainAttachSyntheticEthernetAdapter(domain, dev->data.net,
                        hostname) < 0)
                goto cleanup;
            break;
        case VIR_DOMAIN_DEVICE_CHR:
            if (hyperv2DomainAttachSerial(domain, dev->data.chr) < 0)
                goto cleanup;
            break;
        default:
            /* unsupported device type */
            virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Attaching devices of type %d is not implemented"), dev->type);
            goto cleanup;
    }

    result = 0;

cleanup:
    VIR_FREE(xmlDomain);
    virDomainDefFree(def);
    virDomainDeviceDefFree(dev);
    hypervFreeObject(priv, (hypervObject *) vssd);
    hypervFreeObject(priv, (hypervObject *) rasd);
    hypervFreeObject(priv, (hypervObject *) host);
    return result;
}

int
hyperv2DomainGetAutostart(virDomainPtr domain, int *autostart)
{
    int result = -1;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;


    virUUIDFormat(domain->uuid, uuid_string);
    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    *autostart = (vssd->data->AutomaticStartupAction > 2);
    result = 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *) vssd);
    return result;
}

int
hyperv2DomainSetAutostart(virDomainPtr domain, int autostart)
{
    int result = -1;
    invokeXmlParam *params = NULL;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    properties_t *tab_props = NULL;
    embeddedParam embeddedparam;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";

    virUUIDFormat(domain->uuid, uuid_string);

    /* prepare embedded param */
    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    embeddedparam.nbProps = 2;
    if (VIR_ALLOC_N(tab_props, embeddedparam.nbProps) < 0)
        goto cleanup;
    tab_props[0].name = "AutomaticStartupAction";
    tab_props[0].val = autostart ? "4" : "2";
    tab_props[1].name = "InstanceID";
    tab_props[1].val = vssd->data->InstanceID;

    embeddedparam.instanceName = MSVM_VIRTUALSYSTEMSETTINGDATA_V2_CLASSNAME;
    embeddedparam.prop_t = tab_props;

    /* set up and invoke method */
    if (VIR_ALLOC_N(params, 1) < 0)
        goto cleanup;
    params[0].name = "SystemSettings";
    params[0].type = EMBEDDED_PARAM;
    params[0].param = &embeddedparam;

    result = hyperv2InvokeMethod(priv, params, 1, "ModifySystemSettings",
            MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V2_RESOURCE_URI, selector, NULL);

cleanup:
    hypervFreeObject(priv, (hypervObject *) vssd);
    VIR_FREE(tab_props);
    VIR_FREE(params);
    return result;
}


char *
hyperv2DomainGetSchedulerType(virDomainPtr domain ATTRIBUTE_UNUSED, int *nparams)
{
    char *type;

    if (VIR_STRDUP(type, "allocation") < 0) {
        virReportOOMError();
        return NULL;
    }

    if (nparams)
        *nparams = 3; /* reservation, limit, weight */

    return type;
}

int
hyperv2DomainGetSchedulerParameters(virDomainPtr domain,
        virTypedParameterPtr params, int *nparams)
{
    return hyperv2DomainGetSchedulerParametersFlags(domain, params, nparams,
            VIR_DOMAIN_AFFECT_CURRENT);
}

int
hyperv2DomainGetSchedulerParametersFlags(virDomainPtr domain,
        virTypedParameterPtr params, int *nparams, unsigned int flags)
{
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    Msvm_VirtualSystemSettingData_V2 *vssd = NULL;
    Msvm_ProcessorSettingData_V2 *proc_sd = NULL;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    int saved_nparams = 0;
    int result = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_TYPED_PARAM_STRING_OKAY, -1);

    /* we don't return strings */
    flags &= ~VIR_TYPED_PARAM_STRING_OKAY;

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    /* get info from host */
    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv2GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv2GetProcSDByVSSDInstanceId(priv, vssd->data->InstanceID,
                &proc_sd) < 0)
        goto cleanup;

    /* parse it all out */
    if (virTypedParameterAssign(&params[0], VIR_DOMAIN_SCHEDULER_LIMIT,
                                VIR_TYPED_PARAM_LLONG, proc_sd->data->Limit) < 0)
        goto cleanup;
    saved_nparams++;

    if (*nparams > saved_nparams) {
        if (virTypedParameterAssign(&params[1], VIR_DOMAIN_SCHEDULER_RESERVATION,
                                    VIR_TYPED_PARAM_LLONG, proc_sd->data->Reservation) < 0)
            goto cleanup;
        saved_nparams++;
    }

    if (*nparams > saved_nparams) {
        if (virTypedParameterAssign(&params[2], VIR_DOMAIN_SCHEDULER_WEIGHT,
                                    VIR_TYPED_PARAM_UINT, proc_sd->data->Weight) < 0)
            goto cleanup;
        saved_nparams++;
    }

    *nparams = saved_nparams;

    result = 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *) computerSystem);
    hypervFreeObject(priv, (hypervObject *) vssd);
    hypervFreeObject(priv, (hypervObject *) proc_sd);
    return result;
}

unsigned long long
hyperv2NodeGetFreeMemory(virConnectPtr conn)
{
    unsigned long long res = 0;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Win32_OperatingSystem *operatingSystem = NULL;

    /* Get Win32_OperatingSystem */
    virBufferAddLit(&query, WIN32_OPERATINGSYSTEM_WQL_SELECT);

    if (hypervGetWin32OperatingSystemList(priv, &query, &operatingSystem) < 0)
        goto cleanup;

    if (operatingSystem == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get Win32_OperatingSystem"));
        goto cleanup;
    }

    /* Return free memory in bytes */
    res = operatingSystem->data->FreePhysicalMemory * 1024;

 cleanup:
    hypervFreeObject(priv, (hypervObject *) operatingSystem);
    virBufferFreeAndReset(&query);
    return res;
}

int
hyperv2DomainIsActive(virDomainPtr domain)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    result = hyperv2IsMsvmComputerSystemActive(computerSystem, NULL) ? 1 : 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv2DomainManagedSave(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    bool in_transition = false;

    virCheckFlags(0, -1);

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (!hyperv2IsMsvmComputerSystemActive(computerSystem, &in_transition) ||
        in_transition) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not active or is in state transition"));
        goto cleanup;
    }

    result = hyperv2InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_SUSPENDED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv2DomainHasManagedSaveImage(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    result = computerSystem->data->EnabledState ==
             MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SUSPENDED ? 1 : 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv2DomainManagedSaveRemove(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (computerSystem->data->EnabledState !=
        MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SUSPENDED) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain has no managed save image"));
        goto cleanup;
    }

    result = hyperv2InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V2_REQUESTEDSTATE_DISABLED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int hyperv2DomainSendKey(virDomainPtr domain, unsigned int codeset,
        unsigned int holdtime ATTRIBUTE_UNUSED, unsigned int *keycodes,
        int nkeycodes, unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1, i = 0, keycode = 0;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    Msvm_Keyboard_V2 *keyboards = NULL;
    invokeXmlParam *params = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    int *translatedKeycodes = NULL;
    char *selector = NULL;
    simpleParam simpleparam;

    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv2MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_ComputerSystem.CreationClassName=\"Msvm_ComputerSystem\","
            "Name=\"%s\"} "
            "where ResultClass = Msvm_Keyboard",
            uuid_string);

    if (hyperv2GetMsvmKeyboardList(priv, &query, &keyboards) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not get keyboards for domain"));
        goto cleanup;
    }

    /* translate keycodes to xt and generate keyup scancodes.
     * this code is shamelessly copied fom the vbox driver. */
    translatedKeycodes = (int *) keycodes;

    for (i = 0; i < nkeycodes; i++) {
        if (codeset != VIR_KEYCODE_SET_WIN32) {
            keycode = virKeycodeValueTranslate(codeset, VIR_KEYCODE_SET_WIN32,
                    translatedKeycodes[i]);

            if (keycode < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("Could not translate keycode"));
                goto cleanup;
            }
            translatedKeycodes[i] = keycode;
        }
    }

    if (virAsprintf(&selector,
                    "CreationClassName=Msvm_Keyboard&DeviceID=%s&"
                    "SystemCreationClassName=Msvm_ComputerSystem&"
                    "SystemName=%s", keyboards->data->DeviceID, uuid_string) < 0)
        goto cleanup;

    /* type the keys */
    for (i = 0; i < nkeycodes; i++) {
        VIR_FREE(params);

        if (VIR_ALLOC_N(params, 1) < 0)
            goto cleanup;

        char keyCodeStr[sizeof(int)*3+2];
        snprintf(keyCodeStr, sizeof(keyCodeStr), "%d", translatedKeycodes[i]);

        simpleparam.value = keyCodeStr;

        params[0].name = "keyCode";
        params[0].type = SIMPLE_PARAM;
        params[0].param = &simpleparam;

        if (hyperv2InvokeMethod(priv, params, 1, "TypeKey",
                    MSVM_KEYBOARD_V2_RESOURCE_URI, selector, NULL) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "Could not press key %d",
                    translatedKeycodes[i]);
            goto cleanup;
        }
    }

    result = 0;

cleanup:
    VIR_FREE(params);
    hypervFreeObject(priv, (hypervObject *) keyboards);
    virBufferFreeAndReset(&query);
    hypervFreeObject(priv, (hypervObject *) computerSystem);
    return result;
}


#define MATCH(FLAG) (flags & (FLAG))
int
hyperv2ConnectListAllDomains(virConnectPtr conn, virDomainPtr **domains,
        unsigned int flags)
{
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_V2 *computerSystemList = NULL;
    Msvm_ComputerSystem_V2 *computerSystem = NULL;
    size_t ndoms;
    virDomainPtr domain;
    virDomainPtr *doms = NULL;
    int count = 0;
    int ret = -1;
    size_t i;

    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_FILTERS_ALL, -1);

    /* check for filter combinations that return no results:
     * persistent: all hyperv guests are persistent
     * snapshot: the driver does not support snapshot management
     * autostart: the driver does not support autostarting guests
     */
    if ((MATCH(VIR_CONNECT_LIST_DOMAINS_TRANSIENT) &&
         !MATCH(VIR_CONNECT_LIST_DOMAINS_PERSISTENT)) ||
        (MATCH(VIR_CONNECT_LIST_DOMAINS_AUTOSTART) &&
         !MATCH(VIR_CONNECT_LIST_DOMAINS_NO_AUTOSTART)) ||
        (MATCH(VIR_CONNECT_LIST_DOMAINS_HAS_SNAPSHOT) &&
         !MATCH(VIR_CONNECT_LIST_DOMAINS_NO_SNAPSHOT))) {
        if (domains && VIR_ALLOC_N(*domains, 1) < 0)
            goto cleanup;

        ret = 0;
        goto cleanup;
    }

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_VIRTUAL);

    /* construct query with filter depending on flags */
    if (!(MATCH(VIR_CONNECT_LIST_DOMAINS_ACTIVE) &&
          MATCH(VIR_CONNECT_LIST_DOMAINS_INACTIVE))) {
        if (MATCH(VIR_CONNECT_LIST_DOMAINS_ACTIVE)) {
            virBufferAddLit(&query, "and ");
            virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_ACTIVE);
        }

        if (MATCH(VIR_CONNECT_LIST_DOMAINS_INACTIVE)) {
            virBufferAddLit(&query, "and ");
            virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V2_WQL_INACTIVE);
        }
    }

    if (hyperv2GetMsvmComputerSystemList(priv, &query,
                                        &computerSystemList) < 0)
        goto cleanup;

    if (domains) {
        if (VIR_ALLOC_N(doms, 1) < 0)
            goto cleanup;
        ndoms = 1;
    }

    for (computerSystem = computerSystemList; computerSystem != NULL;
         computerSystem = computerSystem->next) {

        /* filter by domain state */
        if (MATCH(VIR_CONNECT_LIST_DOMAINS_FILTERS_STATE)) {
            int st = hyperv2MsvmComputerSystemEnabledStateToDomainState(computerSystem);
            if (!((MATCH(VIR_CONNECT_LIST_DOMAINS_RUNNING) &&
                   st == VIR_DOMAIN_RUNNING) ||
                  (MATCH(VIR_CONNECT_LIST_DOMAINS_PAUSED) &&
                   st == VIR_DOMAIN_PAUSED) ||
                  (MATCH(VIR_CONNECT_LIST_DOMAINS_SHUTOFF) &&
                   st == VIR_DOMAIN_SHUTOFF) ||
                  (MATCH(VIR_CONNECT_LIST_DOMAINS_OTHER) &&
                   (st != VIR_DOMAIN_RUNNING &&
                    st != VIR_DOMAIN_PAUSED &&
                    st != VIR_DOMAIN_SHUTOFF))))
                continue;
        }

        /* managed save filter */
        if (MATCH(VIR_CONNECT_LIST_DOMAINS_FILTERS_MANAGEDSAVE)) {
            bool mansave = computerSystem->data->EnabledState ==
                           MSVM_COMPUTERSYSTEM_V2_ENABLEDSTATE_SUSPENDED;

            if (!((MATCH(VIR_CONNECT_LIST_DOMAINS_MANAGEDSAVE) && mansave) ||
                  (MATCH(VIR_CONNECT_LIST_DOMAINS_NO_MANAGEDSAVE) && !mansave)))
                continue;
        }

        if (!doms) {
            count++;
            continue;
        }

        if (VIR_RESIZE_N(doms, ndoms, count, 2) < 0)
            goto cleanup;

        domain = NULL;

        if (hyperv2MsvmComputerSystemToDomain(conn, computerSystem,
                                             &domain) < 0)
            goto cleanup;

        doms[count++] = domain;
    }

    if (doms)
        *domains = doms;
    doms = NULL;
    ret = count;

 cleanup:
    if (doms) {
        for (i = 0; i < count; ++i)
            virObjectUnref(doms[i]);

        VIR_FREE(doms);
    }

    hypervFreeObject(priv, (hypervObject *)computerSystemList);

    return ret;
}
#undef MATCH


