/*
 * hyperv_api_v1.h: core driver functions for Hyper-V API version 1 hosts
 *
 * Copyright (C) 2016 Datto Inc.
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

#include "hyperv_api_v1.h"

#include "virlog.h"
#include "virstring.h"
#include "virkeycode.h"
#include "fdstream.h"

#include "hyperv_driver.h"
#include "hyperv_private.h"
#include "hyperv_util.h"
#include "hyperv_wmi.h"
#include "openwsman.h"

VIR_LOG_INIT("hyperv.hyperv_api_v1")

/*
 * WMI invocation functions
 *
 * functions for invoking WMI methods via SOAP
 */
static int
hyperv1InvokeMethodXml(hypervPrivate *priv, WsXmlDocH xmlDocRoot,
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
    Msvm_ConcreteJob_V1 *job = NULL;
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
                _("Could not lookup %s for %s invocation"), "ReturnValue",
                "RequestStateChange");
        goto cleanup;
    }

    returnValue = ws_xml_get_xpath_value(response, xpath_expr_string);
    VIR_FREE(xpath_expr_string);
    xpath_expr_string = NULL;

    if (!returnValue) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not lookup %s for %s invocation"), "ReturnValue",
                "RequestStateChange");
        goto cleanup;
    }

    if (virStrToLong_i(returnValue, NULL, 10, &returnCode) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not parse return code"));
        goto cleanup;
    }

    if (returnCode == CIM_RETURNCODE_TRANSITION_STARTED) {
        virBufferAsprintf(&xpath_expr_buf,
                "/s:Envelope/s:Body/p:%s_OUTPUT/p:Job/a:ReferenceParameters/"
                "w:SelectorSet/w:Selector[@Name='InstanceID']", methodName);
        xpath_expr_string = virBufferContentAndReset(&xpath_expr_buf);
        if (!xpath_expr_string) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Could not lookup %s for %s invocation"), "ReturnValue",
                    "RequestStateChange");
            goto cleanup;
        }

        /* Get Msvm_ConcreteJob_V1 object */
        instanceID = ws_xml_get_xpath_value(response, xpath_expr_string);
        if (!instanceID) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Could not look up instance ID for RequestStateChange invocation"));
            goto cleanup;
        }

        /* Poll every 100ms until the job completes or fails */
        while (!completed) {
            virBufferAddLit(&query, MSVM_CONCRETEJOB_V1_WQL_SELECT);
            virBufferAsprintf(&query, "where InstanceID = \"%s\"", instanceID);

            if (hyperv1GetMsvmConcreteJobList(priv, &query, &job) < 0)
                goto cleanup;

            if (!job) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                        _("Could not lookup %s for %s invocation"), "ReturnValue",
                        "RequestStateChange");
                goto cleanup;
            }

            /* do things depending on the state */
            jobState = job->data->JobState;
            switch (jobState) {
                case MSVM_CONCRETEJOB_V1_JOBSTATE_NEW:
                case MSVM_CONCRETEJOB_V1_JOBSTATE_STARTING:
                case MSVM_CONCRETEJOB_V1_JOBSTATE_RUNNING:
                case MSVM_CONCRETEJOB_V1_JOBSTATE_SHUTTING_DOWN:
                    hypervFreeObject(priv, (hypervObject *) job);
                    job = NULL;
                    usleep(100 * 1000); /* sleep 100 ms */
                    continue;
                case MSVM_CONCRETEJOB_V1_JOBSTATE_COMPLETED:
                    completed = true;
                    break;
                case MSVM_CONCRETEJOB_V1_JOBSTATE_TERMINATED:
                case MSVM_CONCRETEJOB_V1_JOBSTATE_KILLED:
                case MSVM_CONCRETEJOB_V1_JOBSTATE_EXCEPTION:
                case MSVM_CONCRETEJOB_V1_JOBSTATE_SERVICE:
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
                "RequestStateChange", hypervReturnCodeToString(returnCode),
                returnCode);
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
hyperv1InvokeMethod(hypervPrivate *priv, invokeXmlParam *param_t, int nbParameters,
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
    if (hyperv1InvokeMethodXml(priv, doc, methodName, providerURI, selector,
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
hyperv1GetProcessorsByName(hypervPrivate *priv, const char *name,
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
hyperv1GetActiveVirtualSystemList(hypervPrivate *priv,
        Msvm_ComputerSystem_V1 **computerSystemList)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_VIRTUAL);
    virBufferAddLit(&query, "and ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_ACTIVE);

    if (hyperv1GetMsvmComputerSystemList(priv, &query, computerSystemList) < 0)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

/* gets all the vms including the ones that are marked inactive. */
static int
hyperv1GetInactiveVirtualSystemList(hypervPrivate *priv,
        Msvm_ComputerSystem_V1 **list)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_VIRTUAL);
    virBufferAddLit(&query, "and ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_INACTIVE);

    if (hyperv1GetMsvmComputerSystemList(priv, &query, list) < 0)
        goto cleanup;

    if (*list == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv1GetPhysicalSystemList(hypervPrivate *priv,
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
hyperv1GetVirtualSystemByID(hypervPrivate *priv, int id,
        Msvm_ComputerSystem_V1 **computerSystemList)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and ProcessID = %d", id);

    if (hyperv1GetMsvmComputerSystemList(priv, &query, computerSystemList) < 0)
        goto cleanup;

    if (*computerSystemList == NULL)
        goto cleanup;

    result = 0;

cleanup:
    return result;
}

static int
hyperv1GetVirtualSystemByUUID(hypervPrivate *priv, const char *uuid,
        Msvm_ComputerSystem_V1 **computerSystemList)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and Name = \"%s\"", uuid);

    if (hyperv1GetMsvmComputerSystemList(priv, &query, computerSystemList) < 0)
        goto cleanup;

    if (*computerSystemList == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}


static int
hyperv1GetVirtualSystemByName(hypervPrivate *priv, const char *name,
        Msvm_ComputerSystem_V1 **computerSystemList)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and ElementName = \"%s\"", name);

    if (hyperv1GetMsvmComputerSystemList(priv, &query, computerSystemList) < 0)
        goto cleanup;

    if (*computerSystemList == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv1GetVSSDFromUUID(hypervPrivate *priv, const char *uuid,
        Msvm_VirtualSystemSettingData_V1 **data)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    /* Get Msvm_VirtualSystemSettingData_V1 */
    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_ComputerSystem.CreationClassname=\"Msvm_ComputerSystem\","
            "Name=\"%s\"} "
            "where AssocClass = Msvm_SettingsDefineState "
            "ResultClass = Msvm_VirtualSystemSettingData",
            uuid);

    if (hyperv1GetMsvmVirtualSystemSettingDataList(priv, &query, data) < 0)
        goto cleanup;

    if (*data == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv1GetProcSDByVSSDInstanceId(hypervPrivate *priv, const char *id,
        Msvm_ProcessorSettingData_V1 **data)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    /* Get Msvm_ProcessorSettingData_V1 */
    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
            "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
            "ResultClass = Msvm_ProcessorSettingData",
            id);

    if (hyperv1GetMsvmProcessorSettingDataList(priv, &query, data) < 0)
        goto cleanup;

    if (*data == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv1GetMemSDByVSSDInstanceId(hypervPrivate *priv, const char *id,
        Msvm_MemorySettingData_V1 **data)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    /* Get Msvm_MemorySettingData_V1 */
    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
            "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
            "ResultClass = Msvm_MemorySettingData",
            id);

    if (hyperv1GetMsvmMemorySettingDataList(priv, &query, data) < 0)
        goto cleanup;

    if (*data == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv1GetRASDByVSSDInstanceId(hypervPrivate *priv, const char *id,
        Msvm_ResourceAllocationSettingData_V1 **data)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
            "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
            "ResultClass = Msvm_ResourceAllocationSettingData",
            id);

    if (hyperv1GetMsvmResourceAllocationSettingDataList(priv, &query, data) < 0)
        goto cleanup;

    if (*data == NULL)
        goto cleanup;

    result = 0;

cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv1GetSyntheticEthernetPortSDByVSSDInstanceId(hypervPrivate *priv,
        const char *id, Msvm_SyntheticEthernetPortSettingData_V1 **out)
{
    int result = -1;
    virBuffer query = VIR_BUFFER_INITIALIZER;

    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
            "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
            "ResultClass = Msvm_SyntheticEthernetPortSettingData",
            id);

    if (hyperv1GetMsvmSyntheticEthernetPortSettingDataList(priv, &query,
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
hyperv1LookupHostSystemBiosUuid(hypervPrivate *priv, unsigned char *uuid)
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
hyperv1GetHostSystem(hypervPrivate *priv, Msvm_ComputerSystem_V1 **system)
{
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int result = -1;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_PHYSICAL);

    if (hyperv1GetMsvmComputerSystemList(priv, &query, system) < 0)
        goto cleanup;

    if (*system == NULL)
        goto cleanup;

    result = 0;
cleanup:
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv1InvokeMsvmComputerSystemRequestStateChange(virDomainPtr domain,
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
    Msvm_ConcreteJob_V1 *concreteJob = NULL;
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
    response = wsmc_action_invoke(priv->client, MSVM_COMPUTERSYSTEM_V1_RESOURCE_URI,
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
            virBufferAddLit(&query, MSVM_CONCRETEJOB_V1_WQL_SELECT);
            virBufferAsprintf(&query, "where InstanceID = \"%s\"", instanceID);

            if (hyperv1GetMsvmConcreteJobList(priv, &query, &concreteJob) < 0)
                goto cleanup;

            if (concreteJob == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Could not lookup %s for %s invocation"),
                               "Msvm_ConcreteJob", "RequestStateChange");
                goto cleanup;
            }

            switch (concreteJob->data->JobState) {
              case MSVM_CONCRETEJOB_V1_JOBSTATE_NEW:
              case MSVM_CONCRETEJOB_V1_JOBSTATE_STARTING:
              case MSVM_CONCRETEJOB_V1_JOBSTATE_RUNNING:
              case MSVM_CONCRETEJOB_V1_JOBSTATE_SHUTTING_DOWN:
                hypervFreeObject(priv, (hypervObject *)concreteJob);
                concreteJob = NULL;

                usleep(100 * 1000);
                continue;

              case MSVM_CONCRETEJOB_V1_JOBSTATE_COMPLETED:
                completed = true;
                break;

              case MSVM_CONCRETEJOB_V1_JOBSTATE_TERMINATED:
              case MSVM_CONCRETEJOB_V1_JOBSTATE_KILLED:
              case MSVM_CONCRETEJOB_V1_JOBSTATE_EXCEPTION:
              case MSVM_CONCRETEJOB_V1_JOBSTATE_SERVICE:
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
hyperv1MsvmComputerSystemEnabledStateToDomainState
  (Msvm_ComputerSystem_V1 *computerSystem)
{
    switch (computerSystem->data->EnabledState) {
      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_UNKNOWN:
        return VIR_DOMAIN_NOSTATE;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_ENABLED:
        return VIR_DOMAIN_RUNNING;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_DISABLED:
        return VIR_DOMAIN_SHUTOFF;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_PAUSED:
        return VIR_DOMAIN_PAUSED;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SUSPENDED: /* managed save */
        return VIR_DOMAIN_SHUTOFF;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_STARTING:
      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SNAPSHOTTING:
      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SAVING:
        return VIR_DOMAIN_RUNNING;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_STOPPING:
        return VIR_DOMAIN_SHUTDOWN;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_PAUSING:
      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_RESUMING:
        return VIR_DOMAIN_RUNNING;

      default:
        return VIR_DOMAIN_NOSTATE;
    }
}

static bool
hyperv1IsMsvmComputerSystemActive(Msvm_ComputerSystem_V1 *computerSystem,
                                 bool *in_transition)
{
    if (in_transition != NULL)
        *in_transition = false;

    switch (computerSystem->data->EnabledState) {
      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_UNKNOWN:
        return false;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_ENABLED:
        return true;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_DISABLED:
        return false;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_PAUSED:
        return true;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SUSPENDED: /* managed save */
        return false;

      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_STARTING:
      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SNAPSHOTTING:
      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SAVING:
      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_STOPPING:
      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_PAUSING:
      case MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_RESUMING:
        if (in_transition != NULL)
            *in_transition = true;

        return true;

      default:
        return false;
    }
}

static int
hyperv1MsvmComputerSystemToDomain(virConnectPtr conn,
                                 Msvm_ComputerSystem_V1 *computerSystem,
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

    if (hyperv1IsMsvmComputerSystemActive(computerSystem, NULL)) {
        (*domain)->id = computerSystem->data->ProcessID;
    } else {
        (*domain)->id = -1;
    }

    return 0;
}

static int
hyperv1MsvmComputerSystemFromDomain(virDomainPtr domain,
                                   Msvm_ComputerSystem_V1 **computerSystem)
{
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;

    if (computerSystem == NULL || *computerSystem != NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid argument"));
        return -1;
    }

    virUUIDFormat(domain->uuid, uuid_string);

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and Name = \"%s\"", uuid_string);

    if (hyperv1GetMsvmComputerSystemList(priv, &query, computerSystem) < 0)
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
hyperv1CapsInit(hypervPrivate *priv)
{
    virCapsPtr caps = NULL;
    virCapsGuestPtr guest = NULL;

    caps = virCapabilitiesNew(VIR_ARCH_X86_64, 1, 1);

    if (caps == NULL) {
        virReportOOMError();
        return NULL;
    }

    if (hyperv1LookupHostSystemBiosUuid(priv, caps->host.host_uuid) < 0)
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
hyperv1GetDeviceParentRasdFromDeviceId(const char *parentDeviceId,
        Msvm_ResourceAllocationSettingData_V1 *list,
        Msvm_ResourceAllocationSettingData_V1 **out)
{
    int result = -1;
    Msvm_ResourceAllocationSettingData_V1 *entry = list;
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
hyperv1GetInstanceIDFromXMLResponse(WsXmlDocH response)
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
hyperv1DomainDefParseIDEController(virDomainDefPtr def,
        Msvm_ResourceAllocationSettingData_V1 *ide ATTRIBUTE_UNUSED, int idx)
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
hyperv1DomainDefParseSCSIController(virDomainDefPtr def,
        Msvm_ResourceAllocationSettingData_V1 *scsi ATTRIBUTE_UNUSED, int idx)
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
hyperv1DomainDefParseIDEStorageExtent(virDomainDefPtr def, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V1 **ideControllers,
        Msvm_ResourceAllocationSettingData_V1 *disk_parent,
        Msvm_ResourceAllocationSettingData_V1 *disk_ctrlr)
{
    int i = 0;
    int result = -1;
    int ctrlr_idx = -1;
    int addr = -1;

    /* Find controller index */
    for (i = 0; i < HYPERV1_MAX_IDE_CONTROLLERS; i++) {
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

    addr = atoi(disk_parent->data->Address);
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
hyperv1DomainDefParseSCSIStorageExtent(virDomainDefPtr def, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V1 **scsiControllers,
        Msvm_ResourceAllocationSettingData_V1 *disk_parent,
        Msvm_ResourceAllocationSettingData_V1 *disk_ctrlr)
{
    int i = 0;
    int ctrlr_idx = -1;
    int result = -1;
    int addr = -1;

    /* Find controller index */
    for (i = 0; i < HYPERV1_MAX_SCSI_CONTROLLERS; i++) {
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

    addr = atoi(disk_parent->data->Address);
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
hyperv1DomainDefParseFloppyStorageExtent(virDomainDefPtr def, virDomainDiskDefPtr disk)
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
hyperv1DomainDefParseStorage(virDomainPtr domain, virDomainDefPtr def,
        Msvm_ResourceAllocationSettingData_V1 *rasd)
{
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ResourceAllocationSettingData_V1 *entry = rasd;
    Msvm_ResourceAllocationSettingData_V1 *disk_parent = NULL, *disk_ctrlr = NULL;
    virDomainDiskDefPtr disk = NULL;
    int result = -1;
    int scsi_idx = 0;
    int ide_idx = -1;
    char **conn = NULL;
    char **hostResource = NULL;
    char *hostEscaped = NULL;
    char *driveNumberStr = NULL;
    Msvm_DiskDrive_V1 *diskdrive = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int addr = -1, i = 0, ctrlr_idx = -1;
    Msvm_ResourceAllocationSettingData_V1 *ideControllers[HYPERV1_MAX_IDE_CONTROLLERS];
    Msvm_ResourceAllocationSettingData_V1 *scsiControllers[HYPERV1_MAX_SCSI_CONTROLLERS];

    while (entry != NULL) {
        switch (entry->data->ResourceType) {
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_IDE_CONTROLLER:
                ide_idx = entry->data->Address[0] - '0';
                ideControllers[ide_idx] = entry;
                if (hyperv1DomainDefParseIDEController(def, entry, ide_idx) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "Could not parse IDE controller");
                    goto cleanup;
                }
                break;
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_PARALLEL_SCSI_HBA:
                scsiControllers[scsi_idx++] = entry;
                if (hyperv1DomainDefParseSCSIController(def, entry, scsi_idx-1) < 0) {
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

    /* second pass to parse disks */
    entry = rasd;
    while (entry != NULL) {
        if (entry->data->ResourceType ==
                MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_STORAGE_EXTENT) {
            /* reset some vars */
            disk = NULL;
            conn = NULL;

            if (!(disk = virDomainDiskDefNew(priv->xmlopt))) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "Could not allocate DiskDef");
                goto cleanup;
            }

            /* get disk associated with storage extent */
            if (hyperv1GetDeviceParentRasdFromDeviceId(entry->data->Parent,
                        rasd, &disk_parent) < 0)
                goto cleanup;

            /* get associated controller */
            if (hyperv1GetDeviceParentRasdFromDeviceId(disk_parent->data->Parent,
                        rasd, &disk_ctrlr) < 0)
                goto cleanup;

            /* common fields first */
            disk->src->type = VIR_STORAGE_TYPE_FILE;
            disk->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;

            /* note if it's a CD drive */
            if (STREQ(entry->data->ResourceSubType, "Microsoft Virtual CD/DVD Disk")) {
                disk->device = VIR_DOMAIN_DISK_DEVICE_CDROM;
            } else {
                disk->device = VIR_DOMAIN_DISK_DEVICE_DISK;
            }

            /* copy in the source path */
            if (entry->data->Connection.count < 1)
                goto cleanup;
            conn = entry->data->Connection.data; /* implicit cast */
            if (virDomainDiskSetSource(disk, *conn) < 0)
                goto cleanup;

            /* controller-specific fields */
            switch (disk_ctrlr->data->ResourceType) {
                case MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_PARALLEL_SCSI_HBA:
                    if (hyperv1DomainDefParseSCSIStorageExtent(def, disk,
                            scsiControllers, disk_parent, disk_ctrlr) < 0) {
                        goto cleanup;
                    }
                    break;
                case MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_IDE_CONTROLLER:
                    if (hyperv1DomainDefParseIDEStorageExtent(def, disk,
                                ideControllers, disk_parent, disk_ctrlr) < 0) {
                        goto cleanup;
                    }
                    break;
                case MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_OTHER:
                    if (disk_parent->data->ResourceType ==
                            MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_FLOPPY) {
                        if (hyperv1DomainDefParseFloppyStorageExtent(def, disk) < 0)
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
        } else if (entry->data->ResourceType ==
                MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_DISK) {
            /* code to parse physical disk drives, i.e. LUNs */
            if (STREQ(entry->data->ResourceSubType,
                        "Microsoft Physical Disk Drive")) {
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

                if (hyperv1GetDeviceParentRasdFromDeviceId(entry->data->Parent,
                            rasd, &disk_ctrlr) < 0)
                    goto cleanup;

                /* create disk definition */
                if (!(disk = virDomainDiskDefNew(priv->xmlopt))) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "Could not allocate disk def");
                    goto cleanup;
                }

                hostEscaped = virStringReplace(*hostResource,
                        "\\", "\\\\");
                hostEscaped = virStringReplace(hostEscaped, "\"", "\\\"");

                virBufferAsprintf(&query,
                        "select * from Msvm_DiskDrive where "
                        "__PATH=\"%s\"", hostEscaped);

                if (hyperv1GetMsvmDiskDriveList(priv, &query, &diskdrive) < 0
                        || diskdrive == NULL) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                            _("Could not find Msvm_DiskDrive object"));
                    goto cleanup;
                }

                driveNumberStr = virNumToStr(diskdrive->data->DriveNumber);
                ignore_value(virDomainDiskSetSource(disk, driveNumberStr));

                addr = atoi(entry->data->Address);
                if (addr < 0)
                    goto cleanup;

                switch (disk_ctrlr->data->ResourceType) {
                    case MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_PARALLEL_SCSI_HBA:
                        for (i = 0; i < HYPERV1_MAX_SCSI_CONTROLLERS; i++) {
                            if (disk_ctrlr == scsiControllers[i]) {
                                ctrlr_idx = i;
                                break;
                            }
                        }
                        disk->bus = VIR_DOMAIN_DISK_BUS_SCSI;
                        disk->dst = virIndexToDiskName(ctrlr_idx * 64 + addr, "sd");
                        disk->info.addr.drive.unit = addr;
                        break;
                    case MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_IDE_CONTROLLER:
                        for (i = 0; i < HYPERV1_MAX_IDE_CONTROLLERS; i++) {
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

    result = 0;

cleanup:
    if (result != 0 && disk)
            virDomainDiskDefFree(disk);
    hypervFreeObject(priv, (hypervObject *) diskdrive);
    VIR_FREE(hostEscaped);
    VIR_FREE(driveNumberStr);
    virBufferFreeAndReset(&query);

    return result;
}

static int
hyperv1DomainDefParseSyntheticEthernetAdapter(virDomainDefPtr def,
        Msvm_SyntheticEthernetPortSettingData_V1 *net,
        hypervPrivate *priv)
{
    int result = -1;
    virDomainNetDefPtr ndef = NULL;
    Msvm_SwitchPort_V1 *switchPort = NULL;
    Msvm_VirtualSwitch_V1 *vSwitch = NULL;
    char **switchPortConnection = NULL;
    char *switchPortConnectionEscaped = NULL;
    char *temp = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;

    VIR_DEBUG("Parsing ethernet adapter '%s'", net->data->InstanceID);

    if (net->data->Connection.count < 1)
        goto success;

    if (VIR_ALLOC(ndef) < 0)
        goto cleanup;

    ndef->type = VIR_DOMAIN_NET_TYPE_BRIDGE;
    /* set mac address */
    if (virMacAddrParseHex(net->data->Address, &ndef->mac) < 0)
        goto cleanup;

    /* If there's no switch port connection, then the adapter isn't hooked
     * up to anything and we don't have to do anything more. */
    switchPortConnection = net->data->Connection.data;
    if (*switchPortConnection == NULL) {
        VIR_DEBUG("Adapter not connected to switch");
        goto success;
    }

    /* Now we retrieve the associated Msvm_SwitchPort_V1 and Msvm_VirtualSwitch_V1
     * objects, and use all three to build the XML definition. */
    switchPortConnectionEscaped = virStringReplace(*switchPortConnection,
            "\\", "\\\\");
    switchPortConnectionEscaped = virStringReplace(switchPortConnectionEscaped,
            "\"", "\\\"");

    virBufferAsprintf(&query,
                      "select * from Msvm_SwitchPort where __PATH=\"%s\"",
                      switchPortConnectionEscaped);

    if (hyperv1GetMsvmSwitchPortList(priv, &query, &switchPort) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not retrieve switch port"));
        goto cleanup;
    }
    if (switchPort == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not retrieve switch port"));
        goto cleanup;
    }

    /* Now we use the switch port to jump to the switch */
    virBufferFreeAndReset(&query);
    virBufferAsprintf(&query,
            "select * from Msvm_VirtualSwitch where Name=\"%s\"",
            switchPort->data->SystemName);

    if (hyperv1GetMsvmVirtualSwitchList(priv, &query, &vSwitch) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not retrieve virtual switch"));
        goto cleanup;
    }
    if (vSwitch == NULL) {
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
    hypervFreeObject(priv, (hypervObject *) switchPort);
    hypervFreeObject(priv, (hypervObject *) vSwitch);
    virBufferFreeAndReset(&query);
    return result;
}

static int
hyperv1DomainDefParseEthernet(virDomainPtr domain, virDomainDefPtr def,
        Msvm_SyntheticEthernetPortSettingData_V1 *nets)
{
    int result = -1;
    Msvm_SyntheticEthernetPortSettingData_V1 *entry = nets;
    hypervPrivate *priv = domain->conn->privateData;

    while (entry != NULL) {
        if (entry->data->ResourceType ==
                MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_ETHERNET_ADAPTER) {
            if (hyperv1DomainDefParseSyntheticEthernetAdapter(def, entry, priv) < 0)
                goto cleanup;
        }
        entry = entry->next;
    }

    result = 0;

cleanup:
    return result;
}

static int
hyperv1DomainDefParseSerial(virDomainPtr domain ATTRIBUTE_UNUSED,
        virDomainDefPtr def, Msvm_ResourceAllocationSettingData_V1 *rasd)
{
    int result = -1;
    int port_num = 0;
    char **conn = NULL;
    const char *srcPath = NULL;
    Msvm_ResourceAllocationSettingData_V1 *entry = rasd;
    virDomainChrDefPtr serial = NULL;

    while (entry != NULL) {
        if (entry->data->ResourceType ==
                MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_SERIAL_PORT) {
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
hyperv1DomainAttachSyntheticEthernetAdapter(virDomainPtr domain,
        virDomainNetDefPtr net, char *hostname)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    char switchport_guid_string[VIR_UUID_STRING_BUFLEN];
    char guest_guid_string[VIR_UUID_STRING_BUFLEN];
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    char mac_string[VIR_MAC_STRING_BUFLEN];
    char *macAddrEscaped = NULL;
    char *vswitch_selector = NULL;
    const char *managementservice_selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    char *virtualSystemIdentifiers = NULL;
    char *connection__PATH = NULL;
    unsigned char guid[VIR_UUID_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;
    eprParam virtualSwitch_REF, ComputerSystem_REF;
    simpleParam virtualSwitch_Name, virtualSwitch_FriendlyName,
                virtualSwitch_ScopeOfResidence;
    embeddedParam ResourceSettingData;
    invokeXmlParam *params = NULL;
    properties_t *NewResources = NULL;

    /*
     * step 1: create virtual switch port
     * https://msdn.microsoft.com/en-us/library/cc136782(v=vs.85).aspx
     */
    virBufferAddLit(&query, MSVM_VIRTUALSWITCH_V1_WQL_SELECT);
    virBufferAsprintf(&query, "where Name = \"%s\"", net->data.bridge.brname);

    virtualSwitch_REF.query = &query;
    virtualSwitch_REF.wmiProviderURI = ROOT_VIRTUALIZATION;

    /* generate guid for switchport */
    virUUIDGenerate(guid);
    virUUIDFormat(guid, switchport_guid_string);

    /* build the parameters */
    virtualSwitch_Name.value = switchport_guid_string;
    virtualSwitch_FriendlyName.value = "Dynamic Ethernet Switch Port";
    virtualSwitch_ScopeOfResidence.value = "";

    /* create xml params */
    if (VIR_ALLOC_N(params, 4) < 0)
        goto cleanup;
    params[0].name = "VirtualSwitch";
    params[0].type = EPR_PARAM;
    params[0].param = &virtualSwitch_REF;
    params[1].name = "Name";
    params[1].type = SIMPLE_PARAM;
    params[1].param  = &virtualSwitch_Name;
    params[2].name = "FriendlyName";
    params[2].type = SIMPLE_PARAM;
    params[2].param = &virtualSwitch_FriendlyName;
    params[3].name = "ScopeOfResidence";
    params[3].type = SIMPLE_PARAM;
    params[3].param = &virtualSwitch_ScopeOfResidence;

    /* even though 2008 R2 and 2012 (non-R2) share the same "v1" API, there
     * are subtle differences that still need to be handled
     */
    if (STRPREFIX(priv->winVersion, HYPERV_VERSION_2008)) {
        if (virAsprintf(&vswitch_selector,
                        "CreationClassName=Msvm_VirtualSwitchManagementService&"
                        "Name=nvspwmi&SystemCreationClassName=Msvm_ComputerSystem&"
                        "SystemName=%s", hostname) < 0)
            goto cleanup;
    } else if (VIR_STRDUP(vswitch_selector,
                          "CreationClassName=Msvm_VirtualSwitchManagementService") < 0) {
        goto cleanup;
    }

    if (hyperv1InvokeMethod(priv, params, 4, "CreateSwitchPort",
                MSVM_VIRTUALSWITCHMANAGEMENTSERVICE_V1_RESOURCE_URI,
                vswitch_selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not create port for virtual switch '%s'"),
                net->data.bridge.brname);
        goto cleanup;
    }

    /*
     * step 2: add the new port to the VM, which creates the connection
     * https://msdn.microsoft.com/en-us/library/cc160705(v=vs.85).aspx
     */

    /* generate guid for the adapter to present to the guest */
    virUUIDGenerate(guid);
    virUUIDFormat(guid, guest_guid_string);
    virBufferAsprintf(&query, "{%s}", guest_guid_string);
    virtualSystemIdentifiers = virBufferContentAndReset(&query);

    /* build the __PATH variable of the switch port */
    if (virAsprintf(&connection__PATH, "\\\\%s\\root\\virtualization:"
                "Msvm_SwitchPort.CreationClassName=\"Msvm_SwitchPort\","
                "Name=\"%s\",SystemCreationClassName=\"Msvm_VirtualSwitch\","
                "SystemName=\"%s\"", hostname,
                switchport_guid_string, net->data.bridge.brname) < 0)
        goto cleanup;

    /* get the MAC address */
    virMacAddrFormat(&net->mac, mac_string);
    macAddrEscaped = virStringReplace(mac_string, ":", "");

    /* build the ComputerSystem_REF parameter */
    virUUIDFormat(domain->uuid, uuid_string);
    virBufferFreeAndReset(&query);
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, "where Name = \"%s\"", uuid_string);
    ComputerSystem_REF.query = &query;
    ComputerSystem_REF.wmiProviderURI = ROOT_VIRTUALIZATION;

    /* build ResourceSettingData param */
    ResourceSettingData.nbProps = 7;
    if (VIR_ALLOC_N(NewResources, ResourceSettingData.nbProps) < 0)
        goto cleanup;
    NewResources[0].name = "Connection";
    NewResources[0].val = connection__PATH;
    NewResources[1].name = "ElementName";
    NewResources[1].val = "Network Adapter";
    NewResources[2].name = "VirtualSystemIdentifiers";
    NewResources[2].val = virtualSystemIdentifiers;
    NewResources[3].name = "ResourceType";
    NewResources[3].val = "10";
    NewResources[4].name = "ResourceSubType";
    NewResources[4].val = "Microsoft Synthetic Ethernet Port";
    NewResources[5].name = "StaticMacAddress";
    NewResources[5].val = "true";
    NewResources[6].name = "Address";
    NewResources[6].val = macAddrEscaped;
    ResourceSettingData.instanceName = MSVM_SYNTHETICETHERNETPORTSETTINGDATA_V1_CLASSNAME;
    ResourceSettingData.prop_t = NewResources;

    /* build xml params */
    VIR_FREE(params);
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "TargetSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &ComputerSystem_REF;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &ResourceSettingData;

    /* invoke */
    if (hyperv1InvokeMethod(priv, params, 2, "AddVirtualSystemResources",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI,
                managementservice_selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not attach network"));
        goto cleanup;
    }

    result = 0;

cleanup:
    VIR_FREE(vswitch_selector);
    VIR_FREE(virtualSystemIdentifiers);
    VIR_FREE(connection__PATH);
    VIR_FREE(NewResources);
    VIR_FREE(macAddrEscaped);
    VIR_FREE(params);
    virBufferFreeAndReset(&query);

    return result;
}

static int
hyperv1DomainAttachSerial(virDomainPtr domain, virDomainChrDefPtr serial)
{
    int result = -1;
    const char *selector = "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    char *com_string = NULL;
    Msvm_VirtualSystemSettingData_V1 *vssd = NULL;
    Msvm_ResourceAllocationSettingData_V1 *rasd = NULL;
    Msvm_ResourceAllocationSettingData_V1 *entry = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    eprParam ComputerSystem_REF;
    embeddedParam ResourceSettingData;
    properties_t *props = NULL;
    invokeXmlParam *params = NULL;

    if (virAsprintf(&com_string, "COM %d", serial->target.port) < 0)
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv1GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv1GetRASDByVSSDInstanceId(priv, vssd->data->InstanceID, &rasd) < 0)
        goto cleanup;

    /* find the COM port we're interested in changing */
    entry = rasd;
    while (entry != NULL) {
        if ((entry->data->ResourceType ==
                MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_SERIAL_PORT) &&
                STREQ(entry->data->ElementName, com_string)) {
            /* found our com port */
            break;
        }
        entry = entry->next;
    }

    if (entry == NULL)
        goto cleanup;

    /* build Msvm_ComputerSystem_V1 ref */
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, "where Name = \"%s\"", uuid_string);
    ComputerSystem_REF.query = &query;
    ComputerSystem_REF.wmiProviderURI = ROOT_VIRTUALIZATION;

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
    props[2].val = "17"; /* shouldn't be hardcoded but whatever */
    props[3].name = "ResourceSubType";
    props[3].val = entry->data->ResourceSubType;
    ResourceSettingData.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_CLASSNAME;
    ResourceSettingData.prop_t = props;

    /* build xml params object */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "ComputerSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &ComputerSystem_REF;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &ResourceSettingData;

    if (hyperv1InvokeMethod(priv, params, 2, "ModifyVirtualSystemResources",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI,
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
hyperv1DomainCreateSCSIController(virDomainPtr domain)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    const char *selector =
        "CreationClassname=Msvm_VirtualSystemManagementService";
    virBuffer query = VIR_BUFFER_INITIALIZER;
    properties_t *props = NULL;
    invokeXmlParam *params = NULL;
    embeddedParam ResourceSettingData;
    eprParam ComputerSystem_REF;
    char uuid_string[VIR_UUID_STRING_BUFLEN];


    VIR_DEBUG("Attaching SCSI Controller");
    /*
     * https://msdn.microsoft.com/en-us/library/cc160705(v=vs.85).aspx
     */

    /* prepare ComputerSystem_REF param */
    virUUIDFormat(domain->uuid, uuid_string);
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, "where Name = \"%s\"", uuid_string);
    ComputerSystem_REF.query = &query;
    ComputerSystem_REF.wmiProviderURI = ROOT_VIRTUALIZATION;

    /* build ResourceSettingData param */
    if (VIR_ALLOC_N(props, 3) < 0)
        goto cleanup;
    props[0].name = "ElementName";
    props[0].val = "SCSI Controller";
    props[1].name = "ResourceType";
    props[1].val = "6";
    props[2].name = "ResourceSubType";
    props[2].val = "Microsoft Synthetic SCSI Controller";

    ResourceSettingData.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_CLASSNAME;
    ResourceSettingData.prop_t = props;
    ResourceSettingData.nbProps = 3;

    /* build xml params */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "TargetSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &ComputerSystem_REF;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &ResourceSettingData;

    /* invoke */
    if (hyperv1InvokeMethod(priv, params, 2, "AddVirtualSystemResources",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI,
                selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not attach network"));
        goto cleanup;
    }

    result = 0;

cleanup:
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
hyperv1DomainAttachStorageExtent(virDomainPtr domain, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V1 *controller, const char *hostname)
{
    int result = -1;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;
    invokeXmlParam *params = NULL;
    properties_t *props = NULL;
    eprParam eprparam1, eprparam2;
    embeddedParam embeddedparam1, embeddedparam2;
    char *controller__PATH = NULL;
    char *settings__PATH = NULL;
    char *instance_temp = NULL;
    char *settings_instance_id = NULL;
    WsXmlDocH response = NULL;

    virUUIDFormat(domain->uuid, uuid_string);

    VIR_DEBUG("Now attaching disk image '%s' with address %d to bus %d of type %d",
            disk->src->path, disk->info.addr.drive.unit,
            disk->info.addr.drive.controller, disk->bus);

    /* First we add the settings object */

    /* prepare EPR param */
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, " where Name = \"%s\"", uuid_string);
    eprparam1.query = &query;
    eprparam1.wmiProviderURI = ROOT_VIRTUALIZATION;

    /* generate controller PATH */
    instance_temp = virStringReplace(controller->data->InstanceID, "\\", "\\\\");
    if (virAsprintf(&controller__PATH, "\\\\%s\\root\\virtualization:"
                "Msvm_ResourceAllocationSettingData.InstanceID=\"%s\"",
                hostname, instance_temp) < 0)
        goto cleanup;

    /* create embedded params */
    embeddedparam1.nbProps = 4;
    if (VIR_ALLOC_N(props, embeddedparam1.nbProps) < 0)
        goto cleanup;
    props[0].name = "Parent";
    props[0].val = controller__PATH;
    props[1].name = "Address";
    props[1].val = virNumToStr(disk->info.addr.drive.unit);
    props[2].name = "ResourceType";
    props[2].val = "22";
    props[3].name = "ResourceSubType";
    props[3].val = "Microsoft Synthetic Disk Drive";
    embeddedparam1.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_CLASSNAME;
    embeddedparam1.prop_t = props;

    /* create xml params */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "TargetSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &eprparam1;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam1;

    if (hyperv1InvokeMethod(priv, params, 2, "AddVirtualSystemResources",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI,
                selector, &response) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not add disk"));
        goto cleanup;
    }

    if (!response) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get response"));
        goto cleanup;
    }

    /* step 2: create the virtual disk image */

    /* get instance id of newly created disk drive from xml response */
    settings_instance_id = hyperv1GetInstanceIDFromXMLResponse(response);
    if (!settings_instance_id)
        goto cleanup;

    /* prepare PATH variable */
    VIR_FREE(instance_temp);
    instance_temp = virStringReplace(settings_instance_id, "\\", "\\\\");
    if (virAsprintf(&settings__PATH, "\\\\%s\\root\\virtualization:"
                "Msvm_ResourceAllocationSettingData.InstanceID=\"%s\"",
                hostname, instance_temp) < 0)
        goto cleanup;

    /* prepare embedded param 2 */
    VIR_FREE(props);
    embeddedparam2.nbProps = 4;
    if (VIR_ALLOC_N(props, embeddedparam2.nbProps) < 0)
        goto cleanup;
    props[0].name = "Parent";
    props[0].val = settings__PATH;
    props[1].name = "Connection";
    props[1].val = disk->src->path;
    props[2].name = "ResourceType";
    props[2].val = "21";
    props[3].name = "ResourceSubType";
    props[3].val = "Microsoft Virtual Hard Disk";
    embeddedparam2.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_CLASSNAME;
    embeddedparam2.prop_t = props;

    virBufferFreeAndReset(&query);
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, " where Name = \"%s\"", uuid_string);
    eprparam2.query = &query;
    eprparam2.wmiProviderURI = ROOT_VIRTUALIZATION;

    /* create xml params */
    VIR_FREE(params);
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "TargetSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &eprparam2;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam2;

    if (hyperv1InvokeMethod(priv, params, 2, "AddVirtualSystemResources",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI,
                selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not add disk"));
        goto cleanup;
    }

    result = 0;
cleanup:
    virBufferFreeAndReset(&query);
    VIR_FREE(params);
    VIR_FREE(props);
    VIR_FREE(controller__PATH);
    VIR_FREE(settings__PATH);
    VIR_FREE(instance_temp);
    VIR_FREE(settings_instance_id);
    if (response)
        ws_xml_destroy_doc(response);
    return result;
}

static int
hyperv1DomainAttachPhysicalDisk(virDomainPtr domain, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V1 *controller, const char *hostname)
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
    ssize_t found = 0;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    virBuffer query = VIR_BUFFER_INITIALIZER;
    virBuffer rasdQuery = VIR_BUFFER_INITIALIZER;
    invokeXmlParam *params = NULL;
    properties_t *props = NULL;
    eprParam ComputerSystem_REF;
    embeddedParam embeddedparam;
    Msvm_ResourceAllocationSettingData_V1 *diskdefault = NULL;

    if (strstr(disk->src->path, "NODRIVE"))
        goto success; /* Hyper-V doesn't let you define LUNs with no connection */

    virUUIDFormat(domain->uuid, uuid_string);

    VIR_DEBUG("Now attaching LUN '%s' with address %d to bus %d of type %d",
            disk->src->path, disk->info.addr.drive.unit,
            disk->info.addr.drive.controller, disk->bus);

    /* prepare HostResource */

    /* get Msvm_diskDrive root device ID */
    virBufferAsprintf(&rasdQuery,
            "SELECT * FROM Msvm_ResourceAllocationSettingData "
            "WHERE ResourceSubType = 'Microsoft Physical Disk Drive' "
            "AND InstanceID LIKE '%%Default%%'");

    if (hyperv1GetMsvmResourceAllocationSettingDataList(priv, &rasdQuery,
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
    virBufferAsprintf(&query, "\\\\%s\\root\\virtualization:"
            "Msvm_DiskDrive.CreationClassName=\"Msvm_DiskDrive\","
            "DeviceID=\"%s\",SystemCreationClassName=\"Msvm_ComputerSystem\","
            "SystemName=\"%s\"",
            hostname, builtPath, hostname);
    hostResource = virBufferContentAndReset(&query);

    /* prepare controller's path */
    instance_temp = virStringReplace(controller->data->InstanceID, "\\", "\\\\");
    if (virAsprintf(&controller__PATH, "\\\\%s\\root\\virtualization:"
                "Msvm_ResourceAllocationSettingData.InstanceID=\"%s\"",
                hostname, instance_temp) < 0) {
        goto cleanup;
    }

    /* prepare EPR param */
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, " where Name = \"%s\"", uuid_string);
    ComputerSystem_REF.query = &query;
    ComputerSystem_REF.wmiProviderURI = ROOT_VIRTUALIZATION;

    /* create embedded param */
    embeddedparam.nbProps = 5;
    if (VIR_ALLOC_N(props, embeddedparam.nbProps) < 0)
        goto cleanup;
    props[0].name = "Parent";
    props[0].val = controller__PATH;
    props[1].name = "Address";
    props[1].val = virNumToStr(disk->info.addr.drive.unit);
    props[2].name = "ResourceType";
    props[2].val = "22";
    props[3].name = "ResourceSubType";
    props[3].val = "Microsoft Physical Disk Drive";
    props[4].name = "HostResource";
    props[4].val = hostResource;
    embeddedparam.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_CLASSNAME;
    embeddedparam.prop_t = props;

    /* create xml param */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "TargetSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &ComputerSystem_REF;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam;

    if (hyperv1InvokeMethod(priv, params, 2, "AddVirtualSystemResources",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI,
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
    VIR_FREE(hostResource);
    VIR_FREE(instance_temp);
    virBufferFreeAndReset(&query);
    virBufferFreeAndReset(&rasdQuery);
    VIR_FREE(builtPath);
    virStringListFree(matches);
    hypervFreeObject(priv, (hypervObject *) diskdefault);
    return result;
}

static int
hyperv1DomainAttachCDROM(virDomainPtr domain, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V1 *controller, const char *hostname)
{
    int result = -1;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;
    invokeXmlParam *params = NULL;
    properties_t *props = NULL;
    WsXmlDocH response = NULL;
    char *instance_temp = NULL;
    char *controller__PATH = NULL;
    char *settings__PATH = NULL;
    char *settings_instance_id = NULL;
    eprParam ComputerSystem_REF;
    embeddedParam embeddedparam1, embeddedparam2;

    virUUIDFormat(domain->uuid, uuid_string);

    VIR_DEBUG("Now attaching CD/DVD '%s' with address %d to bus %d of type %d",
            disk->src->path, disk->info.addr.drive.unit,
            disk->info.addr.drive.controller, disk->bus);

    /* prepare EPR param */
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, " where Name = \"%s\"", uuid_string);
    ComputerSystem_REF.query = &query;
    ComputerSystem_REF.wmiProviderURI = ROOT_VIRTUALIZATION;

    /* generate controller PATH */
    instance_temp = virStringReplace(controller->data->InstanceID, "\\", "\\\\");
    if (virAsprintf(&controller__PATH, "\\\\%s\\root\\virtualization:"
                "Msvm_ResourceAllocationSettingData.InstanceID=\"%s\"",
                hostname, instance_temp) < 0)
        goto cleanup;

    /* create embedded param for synthetic DVD drive */
    embeddedparam1.nbProps = 4;
    if (VIR_ALLOC_N(props, embeddedparam1.nbProps) < 0)
        goto cleanup;
    props[0].name = "Parent";
    props[0].val = controller__PATH;
    props[1].name = "Address";
    props[1].val = virNumToStr(disk->info.addr.drive.unit);
    props[2].name = "ResourceType";
    props[2].val = "16";
    props[3].name = "ResourceSubType";
    props[3].val = "Microsoft Synthetic DVD Drive";
    embeddedparam1.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_CLASSNAME;
    embeddedparam1.prop_t = props;

    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "TargetSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &ComputerSystem_REF;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam1;

    if (hyperv1InvokeMethod(priv, params, 2, "AddVirtualSystemResources",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI,
                selector, &response) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not add DVD drive"));
        goto cleanup;
    }

    if (!response) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get response"));
        goto cleanup;
    }

    /* step 2: put an ISO into the virtual DVD drive we just created */

    settings_instance_id = hyperv1GetInstanceIDFromXMLResponse(response);
    if (!settings_instance_id)
        goto cleanup;

    /* get instance id of newly created DVD drive from xml response */
    settings_instance_id = hyperv1GetInstanceIDFromXMLResponse(response);
    if (!settings_instance_id)
        goto cleanup;

    /* prepare parent reference */
    VIR_FREE(instance_temp);
    instance_temp = virStringReplace(settings_instance_id, "\\", "\\\\");
    if (virAsprintf(&settings__PATH, "\\\\%s\\root\\virtualization:"
                "Msvm_ResourceAllocationSettingData.InstanceID=\"%s\"",
                hostname, instance_temp) < 0)
        goto cleanup;

    /* refresh buffer */
    virBufferFreeAndReset(&query);
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, " where Name = \"%s\"", uuid_string);

    /* prepare embedded param 2 */
    VIR_FREE(props);
    embeddedparam2.nbProps = 4;
    if (VIR_ALLOC_N(props, embeddedparam2.nbProps) < 0)
        goto cleanup;
    props[0].name = "Parent";
    props[0].val = settings__PATH;
    props[1].name = "Connection";
    props[1].val = disk->src->path;
    props[2].name = "ResourceType";
    props[2].val = "21";
    props[3].name = "ResourceSubType";
    props[3].val = "Microsoft Virtual CD/DVD Disk";
    embeddedparam2.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_CLASSNAME;
    embeddedparam2.prop_t = props;

    /* prepare XML params */
    VIR_FREE(params);
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "TargetSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &ComputerSystem_REF;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam2;

    if (hyperv1InvokeMethod(priv, params, 2, "AddVirtualSystemResources",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI,
                selector, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not add disk"));
        goto cleanup;
    }

    result = 0;
cleanup:
    VIR_FREE(params);
    VIR_FREE(controller__PATH);
    VIR_FREE(settings__PATH);
    VIR_FREE(instance_temp);
    VIR_FREE(settings_instance_id);
    VIR_FREE(props);
    virBufferFreeAndReset(&query);
    if (response)
        ws_xml_destroy_doc(response);
    return result;

}

static int
hyperv1DomainAttachFloppy(virDomainPtr domain, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V1 *driveSettings, const char *hostname)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;
    invokeXmlParam *params = NULL;
    properties_t *props = NULL;
    embeddedParam embeddedparam;
    eprParam eprparam;
    char *instance_temp = NULL;
    char *settings__PATH = NULL;

    virUUIDFormat(domain->uuid, uuid_string);

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
    props[1].name = "Connection";
    props[1].val = disk->src->path;
    props[2].name = "ResourceType";
    props[2].val = "21";
    props[3].name = "ResourceSubType";
    props[3].val = "Microsoft Virtual Floppy Disk";
    embeddedparam.instanceName = MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_CLASSNAME;
    embeddedparam.prop_t = props;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, " where Name = \"%s\"", uuid_string);
    eprparam.query = &query;
    eprparam.wmiProviderURI = ROOT_VIRTUALIZATION;

    /* create xml params */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "TargetSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &eprparam;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam;

    if (hyperv1InvokeMethod(priv, params, 2, "AddVirtualSystemResources",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI,
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
    return result;
}

static int
hyperv1DomainAttachStorageVolume(virDomainPtr domain, virDomainDiskDefPtr disk,
        Msvm_ResourceAllocationSettingData_V1 *controller, const char *hostname)
{
    switch (disk->device) {
        case VIR_DOMAIN_DISK_DEVICE_DISK:
            if (disk->src->type == VIR_STORAGE_TYPE_FILE) {
                return hyperv1DomainAttachStorageExtent(domain, disk, controller,
                        hostname);
            } else if (disk->src->type == VIR_STORAGE_TYPE_BLOCK) {
                return hyperv1DomainAttachPhysicalDisk(domain, disk, controller,
                        hostname);
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid disk type"));
                return -1;
            }
        case VIR_DOMAIN_DISK_DEVICE_CDROM:
            return hyperv1DomainAttachCDROM(domain, disk, controller, hostname);
        default:
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid disk bus"));
            return -1;
    }
}

static int
hyperv1DomainAttachStorage(virDomainPtr domain, virDomainDefPtr def,
        const char *hostname)
{
    int result = -1;
    int num_scsi_controllers = 0;
    int i = 0;
    int ctrlr_idx = -1;
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    Msvm_VirtualSystemSettingData_V1 *vssd = NULL;
    Msvm_ResourceAllocationSettingData_V1 *rasd = NULL;
    Msvm_ResourceAllocationSettingData_V1 *entry = NULL;
    Msvm_ResourceAllocationSettingData_V1 *ideControllers[HYPERV1_MAX_IDE_CONTROLLERS];
    Msvm_ResourceAllocationSettingData_V1 *scsiControllers[HYPERV1_MAX_SCSI_CONTROLLERS];
    Msvm_ResourceAllocationSettingData_V1 *floppySettings = NULL;

    virUUIDFormat(domain->uuid, uuid_string);

    /* start with attaching scsi controllers */
    for (i = 0; i < def->ncontrollers; i++) {
        if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI) {
            /* we have a scsi controller */
            if (hyperv1DomainCreateSCSIController(domain) < 0)
                goto cleanup;
        }
    }

    /*
     * filter through all the rasd entries and isolate our controllers
     */
    if (hyperv1GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv1GetRASDByVSSDInstanceId(priv, vssd->data->InstanceID, &rasd) < 0)
        goto cleanup;

    entry = rasd;
    while (entry != NULL) {
        switch (entry->data->ResourceType) {
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_IDE_CONTROLLER:
                ideControllers[entry->data->Address[0] - '0'] = entry;
                break;
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_PARALLEL_SCSI_HBA:
                scsiControllers[num_scsi_controllers++] = entry;
                break;
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_FLOPPY:
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
                if (hyperv1DomainAttachStorageVolume(domain, def->disks[i],
                            ideControllers[ctrlr_idx], hostname) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                            _("Could not attach disk to IDE controller"));
                    goto cleanup;
                }
                break;
            case VIR_DOMAIN_DISK_BUS_SCSI:
                /* scsi disk */
                if (hyperv1DomainAttachStorageVolume(domain, def->disks[i],
                            scsiControllers[ctrlr_idx], hostname) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                            _("Could not attach disk to SCSI controller"));
                    goto cleanup;
                }
                break;
            case VIR_DOMAIN_DISK_BUS_FDC:
                /* floppy disk */
                if (hyperv1DomainAttachFloppy(domain, def->disks[i],
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
hyperv1ConnectGetType(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return "Hyper-V";
}

int
hyperv1ConnectGetVersion(virConnectPtr conn, unsigned long *version)
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
hyperv1ConnectGetHostname(virConnectPtr conn)
{
    char *hostname = NULL;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Win32_ComputerSystem *computerSystem = NULL;

    virBufferAddLit(&query, WIN32_COMPUTERSYSTEM_WQL_SELECT);

    if (hyperv1GetPhysicalSystemList(priv, &computerSystem) < 0)
        goto cleanup;

    ignore_value(VIR_STRDUP(hostname, computerSystem->data->DNSHostName));

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return hostname;
}

int
hyperv1ConnectGetMaxVcpus(virConnectPtr conn, const char *type ATTRIBUTE_UNUSED)
{
    int result = -1;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ProcessorSettingData_V1 *processorSettingData = NULL;

    /* Get max processors definition */
    virBufferAddLit(&query, "SELECT * FROM Msvm_ProcessorSettingData "
            "WHERE InstanceID LIKE 'Microsoft:Definition%Maximum'");

    if (hyperv1GetMsvmProcessorSettingDataList(priv, &query,
                &processorSettingData) < 0) {
        goto cleanup;
    }

    if (processorSettingData == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not get maximum definition of Msvm_ProcessorSettingData"));
        goto cleanup;
    }

    result = processorSettingData->data->SocketCount * processorSettingData->data->ProcessorsPerSocket;

cleanup:
    hypervFreeObject(priv, (hypervObject *) processorSettingData);
    virBufferFreeAndReset(&query);

    return result;
}

int
hyperv1NodeGetInfo(virConnectPtr conn, virNodeInfoPtr info)
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

    if (hyperv1GetProcessorsByName(priv, computerSystem->data->Name,
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
hyperv1ConnectListDomains(virConnectPtr conn, int *ids, int maxids)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystemList = NULL;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    int count = 0;

    if (maxids == 0)
        return 0;

    if (hyperv1GetActiveVirtualSystemList(priv, &computerSystemList) < 0)
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
hyperv1ConnectNumOfDomains(virConnectPtr conn)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystemList = NULL;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    int count = 0;

    if (hyperv1GetActiveVirtualSystemList(priv, &computerSystemList) < 0)
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
hyperv1DomainCreateXML(virConnectPtr conn, const char *xmlDesc,
        unsigned int flags)
{
    virDomainPtr domain;

    virCheckFlags(VIR_DOMAIN_START_PAUSED | VIR_DOMAIN_START_AUTODESTROY, NULL);

    /* create the new domain */
    domain = hyperv1DomainDefineXML(conn, xmlDesc);
    if (domain == NULL)
        return NULL;

    /* start the domain */
    if (hyperv1InvokeMsvmComputerSystemRequestStateChange(domain,
                MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_ENABLED) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not start domain %s"), domain->name);
        return domain;
    }

    /* If VIR_DOMAIN_START_PAUSED is set, the guest domain will be started, but
     * its CPUs will remain paused */
    if (flags & VIR_DOMAIN_START_PAUSED) {
        /* TODO: use hyperv1DomainSuspend to implement this */
    }

    if (flags & VIR_DOMAIN_START_AUTODESTROY) {
        /* TODO: make auto destroy happen */
    }

    return domain;
}

virDomainPtr
hyperv1DomainLookupByID(virConnectPtr conn, int id)
{
    virDomainPtr domain = NULL;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;

    if (hyperv1GetVirtualSystemByID(priv, id, &computerSystem) < 0) {
        virReportError(VIR_ERR_NO_DOMAIN, _("No domain with ID %d"), id);
        goto cleanup;
    }

    hyperv1MsvmComputerSystemToDomain(conn, computerSystem, &domain);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return domain;
}

virDomainPtr
hyperv1DomainLookupByUUID(virConnectPtr conn, const unsigned char *uuid)
{
    virDomainPtr domain = NULL;
    hypervPrivate *priv = conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    Msvm_ComputerSystem_V1 *computerSystem = NULL;

    virUUIDFormat(uuid, uuid_string);

    if (hyperv1GetVirtualSystemByUUID(priv, uuid_string, &computerSystem) < 0) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("No domain with UUID %s"), uuid_string);
        goto cleanup;
    }

    hyperv1MsvmComputerSystemToDomain(conn, computerSystem, &domain);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return domain;
}

virDomainPtr
hyperv1DomainLookupByName(virConnectPtr conn, const char *name)
{
    virDomainPtr domain = NULL;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;

    if (hyperv1GetVirtualSystemByName(priv, name, &computerSystem) < 0) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not active or is in state transition"));
        goto cleanup;
    }

    hyperv1MsvmComputerSystemToDomain(conn, computerSystem, &domain);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return domain;
}

int
hyperv1DomainSuspend(virDomainPtr domain)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (computerSystem->data->EnabledState !=
        MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_ENABLED) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not active"));
        goto cleanup;
    }

    result = hyperv1InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_PAUSED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv1DomainResume(virDomainPtr domain)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (computerSystem->data->EnabledState !=
        MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_PAUSED) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not paused"));
        goto cleanup;
    }

    result = hyperv1InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_ENABLED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv1DomainShutdown(virDomainPtr domain)
{
    return hyperv1DomainShutdownFlags(domain, 0);
}

int
hyperv1DomainShutdownFlags(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    Msvm_ShutdownComponent_V1 *shutdown = NULL;
    bool in_transition = false;
    char uuid[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;
    simpleParam force, reason;
    invokeXmlParam *params;
    char *selector = NULL;

    virCheckFlags(0, -1);
    virUUIDFormat(domain->uuid, uuid);

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (!hyperv1IsMsvmComputerSystemActive(computerSystem, &in_transition) || in_transition) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                _("Domain is not active or in state transition"));
        goto cleanup;
    }

    virBufferAsprintf(&query,
            "Select * from Msvm_ShutdownComponent "
            "where SystemName = \"%s\"", uuid);

    if (hyperv1GetMsvmShutdownComponentList(priv, &query, &shutdown) < 0 ||
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

    if (hyperv1InvokeMethod(priv, params, 2, "InitiateShutdown",
                MSVM_SHUTDOWNCOMPONENT_V1_RESOURCE_URI, selector, NULL) < 0) {
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
hyperv1DomainReboot(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    result = hyperv1InvokeMsvmComputerSystemRequestStateChange(domain,
            MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_REBOOT);

cleanup:
    hypervFreeObject(priv, (hypervObject *) computerSystem);
    return result;
}

int
hyperv1DomainDestroyFlags(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    bool in_transition = false;

    virCheckFlags(0, -1);

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (!hyperv1IsMsvmComputerSystemActive(computerSystem, &in_transition) ||
        in_transition) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not active or is in state transition"));
        goto cleanup;
    }

    result = hyperv1InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_DISABLED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);
    return result;
}

int
hyperv1DomainDestroy(virDomainPtr domain)
{
    return hyperv1DomainDestroyFlags(domain, 0);
}

char *
hyperv1DomainGetOSType(virDomainPtr domain ATTRIBUTE_UNUSED)
{
    char *osType;

    ignore_value(VIR_STRDUP(osType, "hvm"));
    return osType;
}

unsigned long long
hyperv1DomainGetMaxMemory(virDomainPtr domain)
{
    unsigned long long result = 0;
    bool success = false;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    Msvm_VirtualSystemSettingData_V1 *vssd = NULL;
    Msvm_MemorySettingData_V1 *mem_sd = NULL;
    hypervPrivate *priv = domain->conn->privateData;

    virUUIDFormat(domain->uuid, uuid_string);

    /* get all the data we need */
    if (hyperv1GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv1GetMemSDByVSSDInstanceId(priv, vssd->data->InstanceID,
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
hyperv1DomainSetMaxMemory(virDomainPtr domain, unsigned long memory)
{
    int result = -1;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    invokeXmlParam *params = NULL;
    hypervPrivate *priv = domain->conn->privateData;
    properties_t *tab_props = NULL;
    eprParam eprparam;
    embeddedParam embeddedparam;
    Msvm_VirtualSystemSettingData_V1 *vssd = NULL;
    Msvm_MemorySettingData_V1 *mem_sd = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    char *memory_str = NULL;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";

    unsigned long memory_mb = memory / 1024;

    /* memory has to be multiple of 2 mb; round up if necessary */
    if (memory_mb % 2) memory_mb++;

    if (!(memory_str = virNumToStr(memory_mb)))
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    /* Prepare EPR param */
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, "where Name = \"%s\"", uuid_string);
    eprparam.query = &query;
    eprparam.wmiProviderURI = ROOT_VIRTUALIZATION;

    /* get all the data we need */
    if (hyperv1GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv1GetMemSDByVSSDInstanceId(priv, vssd->data->InstanceID,
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
    embeddedparam.instanceName = MSVM_MEMORYSETTINGDATA_V1_CLASSNAME;
    embeddedparam.prop_t = tab_props;

    /* set up invokeXmlParam */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "ComputerSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &eprparam;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam;

    result = hyperv1InvokeMethod(priv, params, 2, "ModifyVirtualSystemResources",
        MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI, selector, NULL);

cleanup:
    VIR_FREE(tab_props);
    VIR_FREE(params);
    VIR_FREE(memory_str);
    hypervFreeObject(priv, (hypervObject *) vssd);
    hypervFreeObject(priv, (hypervObject *) mem_sd);
    virBufferFreeAndReset(&query);
    return result;
}

int
hyperv1DomainSetMemory(virDomainPtr domain, unsigned long memory)
{
    return hyperv1DomainSetMemoryFlags(domain, memory, 0);
}

int
hyperv1DomainSetMemoryFlags(virDomainPtr domain, unsigned long memory,
        unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    invokeXmlParam *params = NULL;
    properties_t *tab_props = NULL;
    eprParam eprparam;
    embeddedParam embeddedparam;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VirtualSystemSettingData_V1 *vssd = NULL;
    Msvm_MemorySettingData_V1 *mem_sd = NULL;
    char *memory_str = NULL;

    /* memory has to passed as a multiple of 2mb */
    unsigned long memory_mb = memory / 1024;
    if (memory_mb % 2) memory_mb++;

    if (!(memory_str = virNumToStr(memory_mb)))
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    /* Prepare EPR param */
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, "where Name = \"%s\"", uuid_string);
    eprparam.query = &query;
    eprparam.wmiProviderURI = ROOT_VIRTUALIZATION;

    /* get all the data we need */
    if (hyperv1GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv1GetMemSDByVSSDInstanceId(priv, vssd->data->InstanceID,
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
    embeddedparam.instanceName = MSVM_MEMORYSETTINGDATA_V1_CLASSNAME;
    embeddedparam.prop_t = tab_props;

    /* set up invokeXmlParam */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "ComputerSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &eprparam;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam;

    if (hyperv1InvokeMethod(priv, params, 2, "ModifyVirtualSystemResources",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI,
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
    virBufferFreeAndReset(&query);
    return result;
}

int
hyperv1DomainGetInfo(virDomainPtr domain, virDomainInfoPtr info)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    Msvm_VirtualSystemSettingData_V1 *virtualSystemSettingData = NULL;
    Msvm_ProcessorSettingData_V1 *processorSettingData = NULL;
    Msvm_MemorySettingData_V1 *memorySettingData = NULL;

    memset(info, 0, sizeof(*info));

    virUUIDFormat(domain->uuid, uuid_string);

    /* Get Msvm_ComputerSystem_V1 */
    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (hyperv1GetVSSDFromUUID(priv, uuid_string,
                &virtualSystemSettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_VirtualSystemSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    if (hyperv1GetProcSDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &processorSettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_ProcessorSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    if (hyperv1GetMemSDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &memorySettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_MemorySettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Fill struct */
    info->state = hyperv1MsvmComputerSystemEnabledStateToDomainState(computerSystem);
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
hyperv1DomainGetState(virDomainPtr domain, int *state, int *reason,
                     unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    *state = hyperv1MsvmComputerSystemEnabledStateToDomainState(computerSystem);

    if (reason != NULL)
        *reason = 0;

    result = 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

char *
hyperv1DomainScreenshot(virDomainPtr domain,
                        virStreamPtr stream,
                        unsigned int screen ATTRIBUTE_UNUSED,
                        unsigned int flags ATTRIBUTE_UNUSED)
{
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VideoHead_V1 *heads = NULL;
    Msvm_SyntheticDisplayController_V1 *ctrlr = NULL;
    eprParam eprparam;
    simpleParam x_param, y_param;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    invokeXmlParam *params;
    WsXmlDocH ret_doc = NULL;
    int xRes = 640;
    int yRes = 480;
    WsXmlNodeH envelope = NULL;
    WsXmlNodeH body = NULL;
    WsXmlNodeH thumbnail = NULL;
    xmlNodePtr base = NULL;
    xmlNodePtr child = NULL;
    char *imageDataText = NULL;
    char *imageDataBuffer = NULL;
    char thumbnailFilename[VIR_UUID_STRING_BUFLEN + 26];
    uint16_t *bufAs16 = NULL;
    uint16_t px;
    uint8_t *ppmBuffer = NULL;
    char *result = NULL;
    FILE *fd;
    int childCount, pixelCount, i = 0;

    virUUIDFormat(domain->uuid, uuid_string);

    /* get current resolution of VM */
    virBufferAsprintf(&query, "select * from Msvm_SyntheticDisplayController "
                              "where SystemName = \"%s\"", uuid_string);
    if (hyperv1GetMsvmSyntheticDisplayControllerList(priv, &query, &ctrlr) < 0)
        goto thumbnail;

    if (ctrlr == NULL)
        goto thumbnail;

    virBufferFreeAndReset(&query);
    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_SyntheticDisplayController."
            "CreationClassName=\"Msvm_SyntheticDisplayController\","
            "DeviceID=\"%s\","
            "SystemCreationClassName=\"Msvm_ComputerSystem\","
            "SystemName=\"%s\"} "
            "where AssocClass = Msvm_VideoHeadOnController "
            "ResultClass = Msvm_VideoHead",
            ctrlr->data->DeviceID, uuid_string);
    if (hyperv1GetMsvmVideoHeadList(priv, &query, &heads) < 0)
        goto thumbnail;

    if (heads != NULL) {
        xRes = heads->data->CurrentHorizontalResolution;
        yRes = heads->data->CurrentVerticalResolution;
    }

thumbnail:
    /* Prepare EPR param - get Msvm_VirtualSystemSettingData_V1 */
    virBufferFreeAndReset(&query);
    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_ComputerSystem.CreationClassName=\"Msvm_ComputerSystem\","
            "Name=\"%s\"} "
            "where AssocClass = Msvm_SettingsDefineState "
            "ResultClass = Msvm_VirtualSystemSettingData",
            uuid_string);

    eprparam.query = &query;
    eprparam.wmiProviderURI = ROOT_VIRTUALIZATION;

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

    if (hyperv1InvokeMethod(priv, params, 3, "GetVirtualSystemThumbnailImage",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI, selector,
                &ret_doc) < 0)
        goto cleanup;

    envelope = ws_xml_get_soap_envelope(ret_doc);

    if (!envelope) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Could not retrieve thumbnail image"));
        goto cleanup;
    }

    /* Extract the pixel data from XML and save to a file */
    body = ws_xml_get_child(envelope, 1, NULL, NULL);
    thumbnail = ws_xml_get_child(body, 0, NULL, NULL);
    childCount = ws_xml_get_child_count(thumbnail);
    pixelCount = childCount / 2;

    if (VIR_ALLOC_N(imageDataBuffer, childCount + 1) < 0)
        goto cleanup;

    if (VIR_ALLOC_N(ppmBuffer, pixelCount * 3) < 0)
        goto cleanup;

    base = (xmlNodePtr) thumbnail;
    child = base->children;
    while (child) {
        imageDataText = ws_xml_get_node_text((WsXmlNodeH) child);
        imageDataBuffer[i] = (char) atoi(imageDataText);
        child = child->next;
        i++;
    }

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
    if (ctrlr)
        hypervFreeObject(priv, (hypervObject *) ctrlr);
    if (heads)
        hypervFreeObject(priv, (hypervObject *) heads);
    if (ret_doc)
        ws_xml_destroy_doc(ret_doc);
    VIR_FREE(imageDataBuffer);
    VIR_FREE(ppmBuffer);
    VIR_FREE(params);
    return result;
}

int
hyperv1DomainSetVcpus(virDomainPtr domain, unsigned int nvcpus)
{
    return hyperv1DomainSetVcpusFlags(domain, nvcpus, 0);
}

int
hyperv1DomainSetVcpusFlags(virDomainPtr domain, unsigned int nvcpus,
        unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    Msvm_VirtualSystemSettingData_V1 *vssd = NULL;
    Msvm_ProcessorSettingData_V1 *proc_sd = NULL;
    hypervPrivate *priv = domain->conn->privateData;
    eprParam eprparam;
    embeddedParam embeddedparam;
    properties_t *tab_props;
    invokeXmlParam *params = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *nvcpus_str = NULL;

    /* Convert nvcpus into a string value */
    nvcpus_str = virNumToStr(nvcpus);
    if (!nvcpus_str)
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv1GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv1GetProcSDByVSSDInstanceId(priv, vssd->data->InstanceID,
                &proc_sd) < 0) {
        goto cleanup;
    }

    /* prepare parameters */
    virBufferAddLit(&buf, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&buf, "where Name = \"%s\"", uuid_string);
    eprparam.query = &buf;
    eprparam.wmiProviderURI = ROOT_VIRTUALIZATION;

    embeddedparam.nbProps = 2;
    if (VIR_ALLOC_N(tab_props, embeddedparam.nbProps) < 0)
        goto cleanup;
    tab_props[0].name = "VirtualQuantity";
    tab_props[0].val = nvcpus_str;
    tab_props[1].name = "InstanceID";
    tab_props[1].val = proc_sd->data->InstanceID;
    embeddedparam.instanceName = MSVM_PROCESSORSETTINGDATA_V1_CLASSNAME;
    embeddedparam.prop_t = tab_props;

    /* prepare and invoke method */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "ComputerSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &eprparam;
    params[1].name = "ResourceSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam;

    if (hyperv1InvokeMethod(priv, params, 2, "ModifyVirtualSystemResources",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI, selector, NULL) < 0)
        goto cleanup;

    result = 0;

cleanup:
    VIR_FREE(tab_props);
    VIR_FREE(params);
    VIR_FREE(nvcpus_str);
    hypervFreeObject(priv, (hypervObject *) vssd);
    hypervFreeObject(priv, (hypervObject *) proc_sd);
    virBufferFreeAndReset(&buf);
    return result;
}

int
hyperv1DomainGetVcpusFlags(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    Msvm_ProcessorSettingData_V1 *proc_sd = NULL;
    Msvm_VirtualSystemSettingData_V1 *vssd = NULL;

    virCheckFlags(VIR_DOMAIN_VCPU_LIVE |
                  VIR_DOMAIN_VCPU_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM, -1);


    virUUIDFormat(domain->uuid, uuid_string);

    /* Start by getting the Msvm_ComputerSystem_V1 */
    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    /* Check @flags to see if we are to query a running domain, and fail
     * if that domain is not running */
    if (flags & VIR_DOMAIN_VCPU_LIVE) {
        if (computerSystem->data->EnabledState != MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_ENABLED) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s", _("Domain is not active"));
            goto cleanup;
        }
    }

    /* Check @flags to see if we are to return the maximum vCPU limit */
    if (flags & VIR_DOMAIN_VCPU_MAXIMUM) {
        result = hyperv1ConnectGetMaxVcpus(domain->conn, NULL);
        goto cleanup;
    }

    if (hyperv1GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv1GetProcSDByVSSDInstanceId(priv, vssd->data->InstanceID,
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
hyperv1DomainGetVcpus(virDomainPtr domain, virVcpuInfoPtr info, int maxinfo,
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
hyperv1DomainGetMaxVcpus(virDomainPtr dom)
{
    /* If the guest is inactive, this is basically the same as virConnectGetMaxVcpus() */
    return (hyperv1DomainIsActive(dom)) ?
        hyperv1DomainGetVcpusFlags(dom, (VIR_DOMAIN_VCPU_LIVE | VIR_DOMAIN_VCPU_MAXIMUM))
        : hyperv1ConnectGetMaxVcpus(dom->conn, NULL);
}

char *
hyperv1DomainGetXMLDesc(virDomainPtr domain, unsigned int flags)
{
    char *xml = NULL;
    hypervPrivate *priv = domain->conn->privateData;
    virDomainDefPtr def = NULL;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    Msvm_VirtualSystemSettingData_V1 *virtualSystemSettingData = NULL;
    Msvm_ProcessorSettingData_V1 *processorSettingData = NULL;
    Msvm_MemorySettingData_V1 *memorySettingData = NULL;
    Msvm_ResourceAllocationSettingData_V1 *rasd = NULL;
    Msvm_SyntheticEthernetPortSettingData_V1 *nets = NULL;

    /* Flags checked by virDomainDefFormat */

    if (!(def = virDomainDefNew()))
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    /* Get Msvm_ComputerSystem_V1 */
    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (hyperv1GetVSSDFromUUID(priv, uuid_string,
                &virtualSystemSettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_VirtualSystemSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_ProcessorSettingData_V1 */
    if (hyperv1GetProcSDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &processorSettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_ProcessorSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_MemorySettingData_V1 */
    if (hyperv1GetMemSDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &memorySettingData) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_MemorySettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_ResourceAllocationSettingData_V1 */
    if (hyperv1GetRASDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &rasd) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not get resource information for domain %s"),
                computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_SyntheticEthernetPortSettingData_V1 */
    if (hyperv1GetSyntheticEthernetPortSDByVSSDInstanceId(priv,
                virtualSystemSettingData->data->InstanceID,
                &nets) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("Could not get ethernet adapters for domain %s"),
                computerSystem->data->ElementName);
    }

    /* Fill struct */
    def->virtType = VIR_DOMAIN_VIRT_HYPERV;

    if (hyperv1IsMsvmComputerSystemActive(computerSystem, NULL)) {
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

    if (VIR_STRDUP(def->description, virtualSystemSettingData->data->Notes) < 0)
        goto cleanup;

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
    if (hyperv1DomainDefParseStorage(domain, def, rasd) < 0)
        goto cleanup;

    if (hyperv1DomainDefParseSerial(domain, def, rasd) < 0)
        goto cleanup;

    if (hyperv1DomainDefParseEthernet(domain, def, nets) < 0)
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
hyperv1ConnectListDefinedDomains(virConnectPtr conn, char **const names,
        int maxnames)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystemList = NULL;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    int count = 0;
    size_t i;

    if (maxnames == 0)
        return 0;

    if (hyperv1GetInactiveVirtualSystemList(priv, &computerSystemList) < 0)
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
hyperv1ConnectNumOfDefinedDomains(virConnectPtr conn)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystemList = NULL;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    int count = 0;

    if (hyperv1GetInactiveVirtualSystemList(priv, &computerSystemList) < 0)
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
hyperv1DomainCreateWithFlags(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (hyperv1IsMsvmComputerSystemActive(computerSystem, NULL)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is already active or is in state transition"));
        goto cleanup;
    }

    result = hyperv1InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_ENABLED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv1DomainCreate(virDomainPtr domain)
{
    return hyperv1DomainCreateWithFlags(domain, 0);
}

virDomainPtr
hyperv1DomainDefineXML(virConnectPtr conn, const char *xml)
{
    hypervPrivate *priv = conn->privateData;
    virDomainDefPtr def = NULL;
    virDomainPtr domain = NULL;
    invokeXmlParam *params = NULL;
    properties_t *tab_props = NULL;
    embeddedParam embedded_param;
    Msvm_ComputerSystem_V1 *host = NULL;
    int i = 0;
    int nb_params;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    char *hostname = NULL;
    bool success = false;

    if (hyperv1GetHostSystem(priv, &host) < 0)
        goto cleanup;

    hostname = host->data->ElementName;

    /* parse xml */
    def = virDomainDefParseString(xml, priv->caps, priv->xmlopt, NULL,
            1 << VIR_DOMAIN_VIRT_HYPERV | VIR_DOMAIN_XML_INACTIVE);

    if (def == NULL)
        goto cleanup;

    /* create the domain if it doesn't exist already */
    if (def->uuid == NULL ||
            (domain = hyperv1DomainLookupByUUID(conn, def->uuid)) == NULL) {
        /* Prepare params. edit only vm name for now */
        embedded_param.nbProps = 1;
        if (VIR_ALLOC_N(tab_props, embedded_param.nbProps) < 0)
            goto cleanup;

        tab_props[0].name = "ElementName";
        tab_props[0].val = def->name;
        embedded_param.instanceName = MSVM_VIRTUALSYSTEMGLOBALSETTINGDATA_V1_CLASSNAME;
        embedded_param.prop_t = tab_props;

        /* Create XML params for method invocation */
        nb_params = 1;
        if (VIR_ALLOC_N(params, nb_params) < 0)
            goto cleanup;
        params[0].name = "SystemSettingData";
        params[0].type = EMBEDDED_PARAM;
        params[0].param = &embedded_param;

        /* Actually invoke the method to create the VM */
        if (hyperv1InvokeMethod(priv, params, nb_params, "DefineVirtualSystem",
                    MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI,
                    selector, NULL) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Could not create new domain %s"), def->name);
            goto cleanup;
        }

        /* populate a domain ptr so that we can edit it */
        domain = hyperv1DomainLookupByName(conn, def->name);
    }

    /* set domain vcpus */
    if (def->vcpus) {
        if (hyperv1DomainSetVcpus(domain, def->maxvcpus) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("Could not set VM vCPUs"));
            goto cleanup;
        }
    }

    /* Set VM maximum memory */
    if (def->mem.max_memory > 0) {
        if (hyperv1DomainSetMaxMemory(domain, def->mem.max_memory) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not set VM maximum memory"));
            goto cleanup;
        }
    }

    /* Set VM memory */
    if (def->mem.cur_balloon > 0) {
        if (hyperv1DomainSetMemory(domain, def->mem.cur_balloon) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not set VM memory"));
            goto cleanup;
        }
    }

    /* Attach networks */
    for (i = 0; i < def->nnets; i++) {
        if (hyperv1DomainAttachSyntheticEthernetAdapter(domain, def->nets[i],
                    hostname) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                    _("Could not attach network"));
            goto cleanup;
        }
    }

    /* Attach serials */
    for (i = 0; i < def->nserials; i++) {
        if (hyperv1DomainAttachSerial(domain, def->serials[i]) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not attach serial"));
            goto cleanup;
        }
    }

    /* Attach all storage */
    if (hyperv1DomainAttachStorage(domain, def, hostname) < 0) {
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
        ignore_value(hyperv1DomainUndefine(domain));
        return NULL;
    }
}

int
hyperv1DomainUndefine(virDomainPtr domain)
{
    return hyperv1DomainUndefineFlags(domain, 0);
}

int
hyperv1DomainUndefineFlags(virDomainPtr domain, unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1;
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    invokeXmlParam *params = NULL;
    eprParam eprparam;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;

    virCheckFlags(0, -1);
    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    /* try to shut down the VM if it's not disabled, just to be safe */
    if (computerSystem->data->EnabledState != MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_DISABLED) {
        if (hyperv1DomainShutdown(domain) < 0)
            goto cleanup;
    }

    /* prepare params */
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, "where Name = \"%s\"", uuid_string);
    eprparam.query = &query;
    eprparam.wmiProviderURI = ROOT_VIRTUALIZATION;

    if (VIR_ALLOC_N(params, 1) < 0)
        goto cleanup;
    params[0].name = "ComputerSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &eprparam;

    /* actually destroy the vm */
    if (hyperv1InvokeMethod(priv, params, 1, "DestroyVirtualSystem",
                MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI, selector, NULL) < 0) {
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
hyperv1DomainAttachDevice(virDomainPtr domain, const char *xml)
{
    return hyperv1DomainAttachDeviceFlags(domain, xml, 0);
}

int
hyperv1DomainAttachDeviceFlags(virDomainPtr domain, const char *xml,
        unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    char *xmlDomain = NULL;
    virDomainDefPtr def = NULL;
    virDomainDeviceDefPtr dev = NULL;
    Msvm_ComputerSystem_V1 *host = NULL;
    char *hostname = NULL;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    Msvm_ResourceAllocationSettingData_V1 *controller = NULL;
    Msvm_ResourceAllocationSettingData_V1 *rasd = NULL, *entry = NULL;
    Msvm_VirtualSystemSettingData_V1 *vssd = NULL;
    int num_scsi = 0;

    virUUIDFormat(domain->uuid, uuid_string);

    /* get domain definition */
    if ((xmlDomain = hyperv1DomainGetXMLDesc(domain, 0)) == NULL)
        goto cleanup;

    if ((def = virDomainDefParseString(xmlDomain, priv->caps, priv->xmlopt,
                    NULL, 1 << VIR_DOMAIN_VIRT_HYPERV | VIR_DOMAIN_XML_INACTIVE)) == NULL)
        goto cleanup;

    /* get domain device definition */
    if ((dev = virDomainDeviceDefParse(xml, def, priv->caps, priv->xmlopt,
                    VIR_DOMAIN_XML_INACTIVE)) == NULL)
        goto cleanup;

    /* get the host computer system */
    if (hyperv1GetHostSystem(priv, &host) < 0)
        goto cleanup;

    hostname = host->data->ElementName;

    switch (dev->type) {
        case VIR_DOMAIN_DEVICE_DISK:
            /* get our controller
             *
             * TODO: if it turns out that the order is not the same across
             * invocations, implement saving DeviceID of SCSI controllers
             */
            if (hyperv1GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
                goto cleanup;

            if (hyperv1GetRASDByVSSDInstanceId(priv, vssd->data->InstanceID, &rasd) < 0)
                goto cleanup;

            entry = rasd;
            /*
             * The logic here is adapted from the controller identification loop
             * in hyperv1DomainAttachStorage(). This code tries to perform in the
             * same way to make things as consistent as possible.
             */
            switch (dev->data.disk->bus) {
                case VIR_DOMAIN_DISK_BUS_IDE:
                    while (entry != NULL) {
                        if (entry->data->ResourceType ==
                                MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_IDE_CONTROLLER) {
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
                                MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_PARALLEL_SCSI_HBA) {
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
                                MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_FLOPPY) {
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

            if (hyperv1DomainAttachStorageVolume(domain, dev->data.disk,
                        controller, hostname) < 0)
                goto cleanup;
            break;
        case VIR_DOMAIN_DEVICE_NET:
            if (hyperv1DomainAttachSyntheticEthernetAdapter(domain, dev->data.net,
                        hostname) < 0)
                goto cleanup;
            break;
        case VIR_DOMAIN_DEVICE_CHR:
            if (hyperv1DomainAttachSerial(domain, dev->data.chr) < 0)
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
hyperv1DomainGetAutostart(virDomainPtr domain, int *autostart)
{
    int result = -1;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_VirtualSystemGlobalSettingData_V1 *vsgsd = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;


    virUUIDFormat(domain->uuid, uuid_string);
    virBufferAddLit(&query, MSVM_VIRTUALSYSTEMGLOBALSETTINGDATA_V1_WQL_SELECT);
    virBufferAsprintf(&query, "where SystemName = \"%s\"", uuid_string);

    if (hyperv1GetMsvmVirtualSystemGlobalSettingDataList(priv, &query,
                &vsgsd) < 0)
        goto cleanup;

    *autostart = vsgsd->data->AutomaticStartupAction;
    result = 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *) vsgsd);
    virBufferFreeAndReset(&query);
    return result;
}

int
hyperv1DomainSetAutostart(virDomainPtr domain, int autostart)
{
    int result = -1;
    invokeXmlParam *params = NULL;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_VirtualSystemSettingData_V1 *vssd = NULL;
    properties_t *tab_props = NULL;
    eprParam eprparam;
    embeddedParam embeddedparam;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    const char *selector =
        "CreationClassName=Msvm_VirtualSystemManagementService";

    virUUIDFormat(domain->uuid, uuid_string);

    /* Prepare EPR param */
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAsprintf(&query, "where Name = \"%s\"", uuid_string);
    eprparam.query = &query;
    eprparam.wmiProviderURI = ROOT_VIRTUALIZATION;

    /* prepare embedded param */
    if (hyperv1GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    embeddedparam.nbProps = 2;
    if (VIR_ALLOC_N(tab_props, embeddedparam.nbProps) < 0)
        goto cleanup;
    tab_props[0].name = "AutomaticStartupAction";
    tab_props[0].val = autostart ? "2" : "0";
    tab_props[1].name = "InstanceID";
    tab_props[1].val = vssd->data->InstanceID;

    embeddedparam.instanceName = MSVM_VIRTUALSYSTEMGLOBALSETTINGDATA_V1_CLASSNAME;
    embeddedparam.prop_t = tab_props;

    /* set up and invoke method */
    if (VIR_ALLOC_N(params, 2) < 0)
        goto cleanup;
    params[0].name = "ComputerSystem";
    params[0].type = EPR_PARAM;
    params[0].param = &eprparam;
    params[1].name = "SystemSettingData";
    params[1].type = EMBEDDED_PARAM;
    params[1].param = &embeddedparam;

    result = hyperv1InvokeMethod(priv, params, 2, "ModifyVirtualSystem",
            MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_V1_RESOURCE_URI, selector, NULL);

cleanup:
    hypervFreeObject(priv, (hypervObject *) vssd);
    VIR_FREE(tab_props);
    VIR_FREE(params);
    virBufferFreeAndReset(&query);
    return result;
}


char *
hyperv1DomainGetSchedulerType(virDomainPtr domain ATTRIBUTE_UNUSED, int *nparams)
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
hyperv1DomainGetSchedulerParameters(virDomainPtr domain,
        virTypedParameterPtr params, int *nparams)
{
    return hyperv1DomainGetSchedulerParametersFlags(domain, params, nparams,
            VIR_DOMAIN_AFFECT_CURRENT);
}

int
hyperv1DomainGetSchedulerParametersFlags(virDomainPtr domain,
        virTypedParameterPtr params, int *nparams, unsigned int flags)
{
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    Msvm_VirtualSystemSettingData_V1 *vssd = NULL;
    Msvm_ProcessorSettingData_V1 *proc_sd = NULL;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    int saved_nparams = 0;
    int result = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_TYPED_PARAM_STRING_OKAY, -1);

    /* we don't return strings */
    flags &= ~VIR_TYPED_PARAM_STRING_OKAY;

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    /* get info from host */
    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv1GetVSSDFromUUID(priv, uuid_string, &vssd) < 0)
        goto cleanup;

    if (hyperv1GetProcSDByVSSDInstanceId(priv, vssd->data->InstanceID,
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
hyperv1NodeGetFreeMemory(virConnectPtr conn)
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
hyperv1DomainIsActive(virDomainPtr domain)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    result = hyperv1IsMsvmComputerSystemActive(computerSystem, NULL) ? 1 : 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv1DomainManagedSave(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    bool in_transition = false;

    virCheckFlags(0, -1);

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (!hyperv1IsMsvmComputerSystemActive(computerSystem, &in_transition) ||
        in_transition) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not active or is in state transition"));
        goto cleanup;
    }

    result = hyperv1InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_SUSPENDED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv1DomainHasManagedSaveImage(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    result = computerSystem->data->EnabledState ==
             MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SUSPENDED ? 1 : 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hyperv1DomainManagedSaveRemove(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    if (computerSystem->data->EnabledState !=
        MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SUSPENDED) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain has no managed save image"));
        goto cleanup;
    }

    result = hyperv1InvokeMsvmComputerSystemRequestStateChange
               (domain, MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_DISABLED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int hyperv1DomainSendKey(virDomainPtr domain, unsigned int codeset,
        unsigned int holdtime ATTRIBUTE_UNUSED, unsigned int *keycodes,
        int nkeycodes, unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1, i = 0, keycode = 0;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
    Msvm_Keyboard_V1 *keyboards = NULL;
    invokeXmlParam *params = NULL;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    int *translatedKeycodes = NULL;
    char *selector = NULL;
    simpleParam simpleparam;

    virUUIDFormat(domain->uuid, uuid_string);

    if (hyperv1MsvmComputerSystemFromDomain(domain, &computerSystem) < 0)
        goto cleanup;

    virBufferAsprintf(&query,
            "associators of "
            "{Msvm_ComputerSystem.CreationClassName=\"Msvm_ComputerSystem\","
            "Name=\"%s\"} "
            "where ResultClass = Msvm_Keyboard",
            uuid_string);

    if (hyperv1GetMsvmKeyboardList(priv, &query, &keyboards) < 0) {
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

        if (hyperv1InvokeMethod(priv, params, 1, "TypeKey",
                    MSVM_KEYBOARD_V1_RESOURCE_URI, selector, NULL) < 0) {
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
hyperv1ConnectListAllDomains(virConnectPtr conn, virDomainPtr **domains,
        unsigned int flags)
{
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_V1 *computerSystemList = NULL;
    Msvm_ComputerSystem_V1 *computerSystem = NULL;
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

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_VIRTUAL);

    /* construct query with filter depending on flags */
    if (!(MATCH(VIR_CONNECT_LIST_DOMAINS_ACTIVE) &&
          MATCH(VIR_CONNECT_LIST_DOMAINS_INACTIVE))) {
        if (MATCH(VIR_CONNECT_LIST_DOMAINS_ACTIVE)) {
            virBufferAddLit(&query, "and ");
            virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_ACTIVE);
        }

        if (MATCH(VIR_CONNECT_LIST_DOMAINS_INACTIVE)) {
            virBufferAddLit(&query, "and ");
            virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_V1_WQL_INACTIVE);
        }
    }

    if (hyperv1GetMsvmComputerSystemList(priv, &query,
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
            int st = hyperv1MsvmComputerSystemEnabledStateToDomainState(computerSystem);
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
                           MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SUSPENDED;

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

        if (hyperv1MsvmComputerSystemToDomain(conn, computerSystem,
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


