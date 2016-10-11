#include "hyperv_driver_2012.h"
#include "virkeycode.h"

VIR_LOG_INIT("hyperv.hyperv_driver2012");

static int
hypervMsvmComputerSystemEnabledStateToDomainState2012(
    Msvm_ComputerSystem_2012 *computerSystem)
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

static int
hypervMsvmComputerSystemFromDomain2012(virDomainPtr domain,
                                   Msvm_ComputerSystem_2012 **computerSystem)
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

    if (hypervGetMsvmComputerSystem2012List(priv, &query, computerSystem) < 0)
        return -1;

    if (*computerSystem == NULL) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("No domain with UUID %s"), uuid_string);
        return -1;
    }

    return 0;
}

static bool
hypervIsMsvmComputerSystemActive2012(Msvm_ComputerSystem_2012 *computerSystem,
                                 bool *in_transition)
{
    if (in_transition != NULL)
        *in_transition = false;

    switch (computerSystem->data->EnabledState) {
        case MSVM_COMPUTERSYSTEM_2012_ENABLEDSTATE_UNKNOWN:
        case MSVM_COMPUTERSYSTEM_2012_ENABLEDSTATE_DISABLED:
            return false;

        case MSVM_COMPUTERSYSTEM_2012_ENABLEDSTATE_SHUTTING_DOWN:
        case MSVM_COMPUTERSYSTEM_2012_ENABLEDSTATE_STARTING:
            if (in_transition != NULL)
                *in_transition = true;
            return true;

        case MSVM_COMPUTERSYSTEM_2012_ENABLEDSTATE_OTHER:
        case MSVM_COMPUTERSYSTEM_2012_ENABLEDSTATE_ENABLED:
        case MSVM_COMPUTERSYSTEM_2012_ENABLEDSTATE_NOT_APPLICABLE:
        case MSVM_COMPUTERSYSTEM_2012_ENABLEDSTATE_ENABLED_BUT_OFFLINE:
        case MSVM_COMPUTERSYSTEM_2012_ENABLEDSTATE_IN_TEST:
        case MSVM_COMPUTERSYSTEM_2012_ENABLEDSTATE_DEFERRED:
        case MSVM_COMPUTERSYSTEM_2012_ENABLEDSTATE_QUIESCE:
            return true;
    
      default:
        return false;
    }
}

static int
hypervMsvmComputerSystemToDomain2012(virConnectPtr conn,
                                 Msvm_ComputerSystem_2012 *computerSystem,
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

    if (hypervIsMsvmComputerSystemActive2012(computerSystem, NULL)) {
        (*domain)->id = computerSystem->data->ProcessID;
    } else {
        (*domain)->id = -1;
    }

    return 0;
}

int
hypervDomainGetState2012(virDomainPtr domain, int *state, int *reason,
                     unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hypervMsvmComputerSystemFromDomain2012(domain, &computerSystem) < 0)
        goto cleanup;

    *state = hypervMsvmComputerSystemEnabledStateToDomainState2012(computerSystem);

    if (reason != NULL)
        *reason = 0;

    result = 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

#define MATCH(FLAG) (flags & (FLAG))
int
hypervConnectListAllDomains2012(virConnectPtr conn,
                            virDomainPtr **domains,
                            unsigned int flags)
{
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystemList = NULL;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;
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

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_WQL_VIRTUAL);

    /* construct query with filter depending on flags */
    if (!(MATCH(VIR_CONNECT_LIST_DOMAINS_ACTIVE) &&
          MATCH(VIR_CONNECT_LIST_DOMAINS_INACTIVE))) {
        if (MATCH(VIR_CONNECT_LIST_DOMAINS_ACTIVE)) {
            virBufferAddLit(&query, "and ");
            virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_WQL_ACTIVE);
        }

        if (MATCH(VIR_CONNECT_LIST_DOMAINS_INACTIVE)) {
            virBufferAddLit(&query, "and ");
            virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_WQL_INACTIVE);
        }
    }

    if (hypervGetMsvmComputerSystem2012List(priv, &query,
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
            int st = hypervMsvmComputerSystemEnabledStateToDomainState2012(computerSystem);
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
                           MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SUSPENDED;

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

        if (hypervMsvmComputerSystemToDomain2012(conn, computerSystem,
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

virDomainPtr
hypervDomainLookupByID2012(virConnectPtr conn, int id)
{
    virDomainPtr domain = NULL;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and ProcessID = %d", id);

    if (hypervGetMsvmComputerSystem2012List(priv, &query, &computerSystem) < 0)
        goto cleanup;

    if (computerSystem == NULL) {
        virReportError(VIR_ERR_NO_DOMAIN, _("No domain with ID %d"), id);
        goto cleanup;
    }

    hypervMsvmComputerSystemToDomain2012(conn, computerSystem, &domain);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return domain;
}

virDomainPtr
hypervDomainLookupByUUID2012(virConnectPtr conn, const unsigned char *uuid)
{
    virDomainPtr domain = NULL;
    hypervPrivate *priv = conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;

    virUUIDFormat(uuid, uuid_string);

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and Name = \"%s\"", uuid_string);

    if (hypervGetMsvmComputerSystem2012List(priv, &query, &computerSystem) < 0)
        goto cleanup;

    if (computerSystem == NULL) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("No domain with UUID %s"), uuid_string);
        goto cleanup;
    }

    hypervMsvmComputerSystemToDomain2012(conn, computerSystem, &domain);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return domain;
}

virDomainPtr
hypervDomainLookupByName2012(virConnectPtr conn, const char *name)
{
    virDomainPtr domain = NULL;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_WQL_VIRTUAL);
    virBufferAsprintf(&query, "and ElementName = \"%s\"", name);

    if (hypervGetMsvmComputerSystem2012List(priv, &query, &computerSystem) < 0)
        goto cleanup;

    if (computerSystem == NULL) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("No domain with name %s"), name);
        goto cleanup;
    }

    hypervMsvmComputerSystemToDomain2012(conn, computerSystem, &domain);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return domain;
}

static int
hypervInvokeMsvmComputerSystemRequestStateChange2012(virDomainPtr domain,
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
    Msvm_ConcreteJob_2012 *concreteJob = NULL;
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
    response = wsmc_action_invoke(priv->client, MSVM_COMPUTERSYSTEM_2012_RESOURCE_URI,
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
            virBufferAddLit(&query, MSVM_CONCRETEJOB_2012_WQL_SELECT);
            virBufferAsprintf(&query, "where InstanceID = \"%s\"", instanceID);

            if (hypervGetMsvmConcreteJob2012List(priv, &query, &concreteJob) < 0)
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
hypervDomainCreateWithFlags2012(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hypervMsvmComputerSystemFromDomain2012(domain, &computerSystem) < 0)
        goto cleanup;

    if (hypervIsMsvmComputerSystemActive2012(computerSystem, NULL)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is already active or is in state transition"));
        goto cleanup;
    }

    result = hypervInvokeMsvmComputerSystemRequestStateChange2012
               (domain, MSVM_COMPUTERSYSTEM_2012_REQUESTEDSTATE_RUNNING);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hypervDomainCreate2012(virDomainPtr domain)
{
    return hypervDomainCreateWithFlags2012(domain, 0);
}

int
hypervDomainShutdownFlags2012(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;
    bool in_transition = false;

    virCheckFlags(0, -1);

    if (hypervMsvmComputerSystemFromDomain2012(domain, &computerSystem) < 0) {
        goto cleanup;
    }

    if (!hypervIsMsvmComputerSystemActive2012(computerSystem, &in_transition) || in_transition) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not active or is in state transition"));
        goto cleanup;
    }

    result = hypervInvokeMsvmComputerSystemRequestStateChange2012
               (domain, MSVM_COMPUTERSYSTEM_2012_REQUESTEDSTATE_OFF);

 cleanup:
    hypervFreeObject(priv, (hypervObject *) computerSystem);
    return result;
}

int
hypervDomainShutdown2012(virDomainPtr dom)
{
    return hypervDomainShutdownFlags2012(dom, 0);
}

int
hypervDomainDestroyFlags2012(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;
    bool in_transition = false;

    virCheckFlags(0, -1);

    if (hypervMsvmComputerSystemFromDomain2012(domain, &computerSystem) < 0)
        goto cleanup;

    if (!hypervIsMsvmComputerSystemActive2012(computerSystem, &in_transition) ||
        in_transition) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain is not active or is in state transition"));
        goto cleanup;
    }

    result = hypervInvokeMsvmComputerSystemRequestStateChange2012
               (domain, MSVM_COMPUTERSYSTEM_2012_REQUESTEDSTATE_OFF);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}


int
hypervDomainDestroy2012(virDomainPtr domain)
{
    return hypervDomainDestroyFlags2012(domain, 0);
}

int
hypervDomainReboot2012(virDomainPtr domain, unsigned int flags)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;

    virCheckFlags(0, -1);

    if (hypervMsvmComputerSystemFromDomain2012(domain, &computerSystem) < 0)
        goto cleanup;

    result = hypervInvokeMsvmComputerSystemRequestStateChange2012
               (domain, MSVM_COMPUTERSYSTEM_2012_REQUESTEDSTATE_RESET);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hypervDomainIsActive2012(virDomainPtr domain)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;

    if (hypervMsvmComputerSystemFromDomain2012(domain, &computerSystem) < 0)
        goto cleanup;

    result = hypervIsMsvmComputerSystemActive2012(computerSystem, NULL) ? 1 : 0;

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hypervDomainUndefineFlags2012(virDomainPtr domain, unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1, nb_params;
    const char *selector = "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    invokeXmlParam *params = NULL;
    eprParam eprparam;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;

    virCheckFlags(0, -1);

    virUUIDFormat(domain->uuid, uuid_string);

    if (hypervMsvmComputerSystemFromDomain2012(domain, &computerSystem) < 0) {
        goto cleanup;
    }

    /* Shutdown the VM if not disabled */
    if (computerSystem->data->EnabledState != MSVM_COMPUTERSYSTEM_ENABLEDSTATE_DISABLED) {
        if (hypervDomainShutdown2012(domain) < 0) {
            goto cleanup;
        }
    }

    /* Deleting the VM */

    /* Prepare EPR param */
    virBufferFreeAndReset(&query);
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_SELECT);
    virBufferAsprintf(&query, "where Name = \"%s\"", uuid_string);
    eprparam.query = &query;
    eprparam.wmiProviderURI = ROOT_VIRTUALIZATION_V2;

    /* Create invokeXmlParam tab */
    nb_params = 1;
    if (VIR_ALLOC_N(params, nb_params) < 0)
        goto cleanup;
    (*params).name = "AffectedSystem";
    (*params).type = EPR_PARAM;
    (*params).param = &eprparam;

    /* Destroy VM */
    if (hypervInvokeMethod(priv, params, nb_params, "DestroySystem",
                           MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_2012_RESOURCE_URI, selector) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not delete domain"));
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
hypervDomainUndefine2012(virDomainPtr domain)
{
    return hypervDomainUndefineFlags2012(domain, 0);
}

char *
hypervDomainGetXMLDesc2012(virDomainPtr domain, unsigned int flags)
{
    int i;
    char *xml = NULL;
    hypervPrivate *priv = domain->conn->privateData;
    virDomainDefPtr def = NULL;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    const char **notesArr;
    char **noteStrPtr;
    const char *notesDelim = "\n";
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;
    Msvm_VirtualSystemSettingData_2012 *virtualSystemSettingData = NULL;
    Msvm_ProcessorSettingData_2012 *processorSettingData = NULL;
    Msvm_MemorySettingData_2012 *memorySettingData = NULL;
    // Msvm_VirtualHardDiskSettingData_2012 *hardDiskSettingData = NULL;

    /* Flags checked by virDomainDefFormat */

    if (!(def = virDomainDefNew()))
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    /* Get Msvm_ComputerSystem */
    if (hypervMsvmComputerSystemFromDomain2012(domain, &computerSystem) < 0)
        goto cleanup;

    /* Get Msvm_VirtualSystemSettingData */
    virBufferAsprintf(&query,
                      "associators of "
                      "{Msvm_ComputerSystem.CreationClassName=\"Msvm_ComputerSystem\","
                      "Name=\"%s\"} "
                      "where AssocClass = Msvm_SettingsDefineState "
                      "ResultClass = Msvm_VirtualSystemSettingData",
                      uuid_string);

    if (hypervGetMsvmVirtualSystemSettingData2012List(priv, &query,
                                                  &virtualSystemSettingData) < 0) {
        goto cleanup;
    }

    if (virtualSystemSettingData == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_VirtualSystemSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_ProcessorSettingData */
    virBufferAsprintf(&query,
                      "associators of "
                      "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
                      "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
                      "ResultClass = Msvm_ProcessorSettingData",
                      virtualSystemSettingData->data->InstanceID);

    if (hypervGetMsvmProcessorSettingData2012List(priv, &query,
                                              &processorSettingData) < 0) {
        goto cleanup;
    }

    if (processorSettingData == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_ProcessorSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_MemorySettingData */
    virBufferAsprintf(&query,
                      "associators of "
                      "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
                      "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
                      "ResultClass = Msvm_MemorySettingData",
                      virtualSystemSettingData->data->InstanceID);

    if (hypervGetMsvmMemorySettingData2012List(priv, &query,
                                           &memorySettingData) < 0) {
        goto cleanup;
    }


    if (memorySettingData == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_MemorySettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Fill struct */
    def->virtType = VIR_DOMAIN_VIRT_HYPERV;

    if (hypervIsMsvmComputerSystemActive2012(computerSystem, NULL)) {
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

    if (VIR_ALLOC_N(notesArr, virtualSystemSettingData->data->Notes.count + 1) < 0) {
        goto cleanup;
    }
    
    noteStrPtr = virtualSystemSettingData->data->Notes.data;
    
    for (i = 0; i < virtualSystemSettingData->data->Notes.count; i++) {
        notesArr[i] = *noteStrPtr++;
    }
    
    if (VIR_STRDUP(def->description, virStringJoin(notesArr, notesDelim)) < 0)
        goto cleanup;

    virDomainDefSetMemoryTotal(def, memorySettingData->data->Limit * 1024); /* megabyte to kilobyte */
    def->mem.cur_balloon = memorySettingData->data->VirtualQuantity * 1024; /* megabyte to kilobyte */

    if (virDomainDefSetVcpusMax(def,
                                processorSettingData->data->VirtualQuantity,
                                NULL) < 0) {
        goto cleanup;
    }

    if (virDomainDefSetVcpus(def,
                             processorSettingData->data->VirtualQuantity) < 0) {
        goto cleanup;
    }

    def->os.type = VIR_DOMAIN_OSTYPE_HVM;

    /* FIXME: devices section is totally missing */

    xml = virDomainDefFormat(def, NULL,
                             virDomainDefFormatConvertXMLFlags(flags));

 cleanup:
    virDomainDefFree(def);
    hypervFreeObject(priv, (hypervObject *)computerSystem);
    hypervFreeObject(priv, (hypervObject *)virtualSystemSettingData);
    hypervFreeObject(priv, (hypervObject *)processorSettingData);
    hypervFreeObject(priv, (hypervObject *)memorySettingData);

    return xml;
}

int
hypervConnectNumOfDomains2012(virConnectPtr conn)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystemList = NULL;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;
    int count = 0;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_VIRTUAL);
    virBufferAddLit(&query, "and ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_ACTIVE);

    if (hypervGetMsvmComputerSystem2012List(priv, &query,
                                        &computerSystemList) < 0) {
        goto cleanup;
    }

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
hypervConnectListDomains2012(virConnectPtr conn, int *ids, int maxids)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystemList = NULL;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;
    int count = 0;

    if (maxids == 0)
        return 0;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_VIRTUAL);
    virBufferAddLit(&query, "and ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_ACTIVE);

    if (hypervGetMsvmComputerSystem2012List(priv, &query,
                                        &computerSystemList) < 0) {
        goto cleanup;
    }

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
hypervConnectNumOfDefinedDomains2012(virConnectPtr conn)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystemList = NULL;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;
    int count = 0;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_VIRTUAL);
    virBufferAddLit(&query, "and ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_INACTIVE);

    if (hypervGetMsvmComputerSystem2012List(priv, &query,
                                        &computerSystemList) < 0) {
        goto cleanup;
    }

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
hypervConnectListDefinedDomains2012(virConnectPtr conn, char **const names, int maxnames)
{
    bool success = false;
    hypervPrivate *priv = conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystemList = NULL;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;
    int count = 0;
    size_t i;

    if (maxnames == 0)
        return 0;

    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_SELECT);
    virBufferAddLit(&query, "where ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_VIRTUAL);
    virBufferAddLit(&query, "and ");
    virBufferAddLit(&query, MSVM_COMPUTERSYSTEM_2012_WQL_INACTIVE);

    if (hypervGetMsvmComputerSystem2012List(priv, &query,
                                        &computerSystemList) < 0) {
        goto cleanup;
    }

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

virDomainPtr
hypervDomainDefineXML2012(virConnectPtr conn, const char *xml)
{
    hypervPrivate *priv = conn->privateData;
    virDomainDefPtr def = NULL;
    virDomainPtr domain = NULL;
    invokeXmlParam *params = NULL;
    properties_t *tab_props = NULL;
    embeddedParam embeddedparam;
    int nb_params;//, i;
    const char *selector = "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];

    /* Parse XML domain description */
    if ((def = virDomainDefParseString(xml, priv->caps, priv->xmlopt, NULL,
                                       1 << VIR_DOMAIN_VIRT_HYPERV | VIR_DOMAIN_XML_INACTIVE)) == NULL) {
        goto cleanup;
    }

    /* Create the domain if does not exist */
    if (def->uuid == NULL || (domain = hypervDomainLookupByUUID2012(conn, def->uuid)) == NULL) {
        /* Prepare EMBEDDED param */
        /* Edit only VM name */
        /* FIXME: cannot edit VM UUID */
        embeddedparam.nbProps = 1;
        if (VIR_ALLOC_N(tab_props, embeddedparam.nbProps) < 0)
            goto cleanup;
        (*tab_props).name = "ElementName";
        (*tab_props).val = def->name;
        embeddedparam.instanceName = "Msvm_VirtualSystemGlobalSettingData";
        embeddedparam.prop_t = tab_props;

        /* Create invokeXmlParam */
        nb_params = 1;
        if (VIR_ALLOC_N(params, nb_params) < 0)
            goto cleanup;
        (*params).name = "SystemSettings";
        (*params).type = EMBEDDED_PARAM;
        (*params).param = &embeddedparam;

        /* Create VM */
        if (hypervInvokeMethod(priv, params, nb_params, "DefineSystem",
                               MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_2012_RESOURCE_URI, selector) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not create new domain %s"), def->name);
            goto cleanup;
        }

        /* Get domain pointer */
        domain = hypervDomainLookupByName2012(conn, def->name);

        VIR_DEBUG("Domain created: name=%s, uuid=%s",
                  domain->name, virUUIDFormat(domain->uuid, uuid_string));
    }

    /* Set VM maximum memory */
  /*  if (def->mem.max_memory > 0) {
        if (hypervDomainSetMaxMemory(domain, def->mem.max_memory) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not set VM maximum memory"));
        }
    }*/

    /* Set VM memory */
    if (def->mem.cur_balloon > 0) {
        if (hypervDomainSetMemory2012(domain, def->mem.cur_balloon) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not set VM memory"));
        }
    }

    /* Set VM vcpus */
    /*
    if ((int)def->vcpus > 0) {
        if (hypervDomainSetVcpus(domain, def->vcpus) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not set VM vCPUs"));
        }
    }
    */

    /* Attach networks */
  /*  for (i = 0; i < def->nnets; i++) {
        if (hypervDomainAttachNetwork(domain, def->nets[i]) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not attach network"));
        }
    }*/

    /* Attach disks */
/*    for (i = 0; i < def->ndisks; i++) {
        if (hypervDomainAttachDisk(domain, def->disks[i]) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not attach disk"));
        }
    }*/

 cleanup:
    virDomainDefFree(def);
    VIR_FREE(tab_props);
    VIR_FREE(params);

    return domain;
}

int
hypervDomainGetInfo2012(virDomainPtr domain, virDomainInfoPtr info)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;
    Msvm_VirtualSystemSettingData_2012 *virtualSystemSettingData = NULL;
    Msvm_ProcessorSettingData_2012 *processorSettingData = NULL;
    Msvm_MemorySettingData_2012 *memorySettingData = NULL;

    memset(info, 0, sizeof(*info));

    virUUIDFormat(domain->uuid, uuid_string);

    /* Get Msvm_ComputerSystem */
    if (hypervMsvmComputerSystemFromDomain2012(domain, &computerSystem) < 0)
        goto cleanup;

    /* Get Msvm_VirtualSystemSettingData */
    virBufferAsprintf(&query,
                      "associators of "
                      "{Msvm_ComputerSystem.CreationClassName=\"Msvm_ComputerSystem\","
                      "Name=\"%s\"} "
                      "where AssocClass = Msvm_SettingsDefineState "
                      "ResultClass = Msvm_VirtualSystemSettingData",
                      uuid_string);

    if (hypervGetMsvmVirtualSystemSettingData2012List(priv, &query,
                                                  &virtualSystemSettingData) < 0) {
        goto cleanup;
    }

    if (virtualSystemSettingData == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_VirtualSystemSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_ProcessorSettingData */
    virBufferAsprintf(&query,
                      "associators of "
                      "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
                      "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
                      "ResultClass = Msvm_ProcessorSettingData",
                      virtualSystemSettingData->data->InstanceID);

    if (hypervGetMsvmProcessorSettingData2012List(priv, &query,
                                              &processorSettingData) < 0) {
        goto cleanup;
    }

    if (processorSettingData == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_ProcessorSettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Get Msvm_MemorySettingData */
    virBufferAsprintf(&query,
                      "associators of "
                      "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
                      "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
                      "ResultClass = Msvm_MemorySettingData",
                      virtualSystemSettingData->data->InstanceID);

    if (hypervGetMsvmMemorySettingData2012List(priv, &query,
                                           &memorySettingData) < 0) {
        goto cleanup;
    }


    if (memorySettingData == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for domain %s"),
                       "Msvm_MemorySettingData",
                       computerSystem->data->ElementName);
        goto cleanup;
    }

    /* Fill struct */
    info->state = hypervMsvmComputerSystemEnabledStateToDomainState2012(computerSystem);
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
hypervDomainSetMemory2012(virDomainPtr domain, unsigned long memory)
{
    return hypervDomainSetMemoryFlags2012(domain, memory, 0);
}

int
hypervDomainSetMemoryFlags2012(virDomainPtr domain, unsigned long memory,
                               unsigned int flags ATTRIBUTE_UNUSED)
{
    int result = -1, nb_params;
    const char *selector = "CreationClassName=Msvm_VirtualSystemManagementService";
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    invokeXmlParam *params = NULL;
    properties_t *tab_props = NULL;
    embeddedParam embeddedparam;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_VirtualSystemSettingData_2012 *virtualSystemSettingData = NULL;
    Msvm_MemorySettingData_2012 *memorySettingData = NULL;
    unsigned long memory_mb = memory / 1024;   /* Memory converted in MB */
    char *memory_str = NULL;

    /* Memory value must be a multiple of 2 MB; round up it accordingly if necessary */
    if (memory_mb % 2) memory_mb++;

    /* Convert the memory value as a string */
    memory_str = num2str(memory_mb);
    if (memory_str == NULL)
        goto cleanup;

    virUUIDFormat(domain->uuid, uuid_string);

    VIR_DEBUG("memory=%sMb, uuid=%s", memory_str, uuid_string);

    /* Get Msvm_VirtualSystemSettingData */
    virBufferAsprintf(&query,
                      "associators of "
                      "{Msvm_ComputerSystem.CreationClassName=\"Msvm_ComputerSystem\","
                      "Name=\"%s\"} "
                      "where AssocClass = Msvm_SettingsDefineState "
                      "ResultClass = Msvm_VirtualSystemSettingData",
                      uuid_string);
    if (hypervGetMsvmVirtualSystemSettingData2012List(priv, &query, &virtualSystemSettingData) < 0)
        goto cleanup;

    /* Get Msvm_MemorySettingData */
    virBufferFreeAndReset(&query);
    virBufferAsprintf(&query,
                      "associators of "
                      "{Msvm_VirtualSystemSettingData.InstanceID=\"%s\"} "
                      "where AssocClass = Msvm_VirtualSystemSettingDataComponent "
                      "ResultClass = Msvm_MemorySettingData",
                      virtualSystemSettingData->data->InstanceID);
    if (hypervGetMsvmMemorySettingData2012List(priv, &query, &memorySettingData) < 0)
        goto cleanup;

    /* Prepare EMBEDDED param */
    embeddedparam.nbProps = 2;
    if (VIR_ALLOC_N(tab_props, embeddedparam.nbProps) < 0)
        goto cleanup;
    (*tab_props).name = "VirtualQuantity";
    (*tab_props).val = memory_str;
    (*(tab_props+1)).name = "InstanceID";
    (*(tab_props+1)).val = memorySettingData->data->InstanceID;
    embeddedparam.instanceName =  "Msvm_MemorySettingData";
    embeddedparam.prop_t = tab_props;

    /* Create invokeXmlParam */
    nb_params = 1;
    if (VIR_ALLOC_N(params, nb_params) < 0)
        goto cleanup;
    (*params).name = "ResourceSettings";
    (*params).type = EMBEDDED_PARAM;
    (*params).param = &embeddedparam;

    if (hypervInvokeMethod(priv, params, nb_params, "ModifyResourceSettings",
                           MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_2012_RESOURCE_URI, selector) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not set domain memory"));
        goto cleanup;
    }

    result = 0;

 cleanup:
    VIR_FREE(tab_props);
    VIR_FREE(params);
    VIR_FREE(memory_str);
    hypervFreeObject(priv, (hypervObject *)virtualSystemSettingData);
    hypervFreeObject(priv, (hypervObject *)memorySettingData);
    virBufferFreeAndReset(&query);

    return result;
}

int
hypervDomainSendKey2012(virDomainPtr domain,
                    unsigned int codeset,
                    unsigned int holdtime,
                    unsigned int *keycodes,
                    int nkeycodes,
                    unsigned int flags)
{
    int result = -1, nb_params, i;
    char *selector = NULL;    
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    hypervPrivate *priv = domain->conn->privateData;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ComputerSystem_2012 *computerSystem = NULL;
    Msvm_Keyboard_2012 *keyboard = NULL;
    invokeXmlParam *params = NULL;
    int *translatedKeyCodes = NULL;
    int keycode;
    simpleParam simpleparam;

    virCheckFlags(0, -1);
    virUUIDFormat(domain->uuid, uuid_string);

    /* Get computer system */
    if (hypervMsvmComputerSystemFromDomain2012(domain, &computerSystem) < 0)
        goto cleanup;

    /* Get keyboard */
    virBufferAsprintf(&query,
                      "associators of "
                      "{Msvm_ComputerSystem.CreationClassName=\"Msvm_ComputerSystem\","
                      "Name=\"%s\"} "
                      "where ResultClass = Msvm_Keyboard",
                      uuid_string);

    if (hypervGetMsvmKeyboard2012List(priv, &query, &keyboard) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                _("No keyboard for domain with UUID %s"), uuid_string);
        goto cleanup;
    }

    /* Translate keycodes to xt and generate keyup scancodes;
       this is copied from the vbox driver */
    translatedKeyCodes = (int *) keycodes;

    for (i = 0; i < nkeycodes; i++) {
        if (codeset != VIR_KEYCODE_SET_WIN32) {
            keycode = virKeycodeValueTranslate(codeset, VIR_KEYCODE_SET_WIN32,
                                               translatedKeyCodes[i]);
            if (keycode < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot translate keycode %u of %s codeset to"
                                 " win32 keycode"),
                               translatedKeyCodes[i],
                               virKeycodeSetTypeToString(codeset));
                goto cleanup;
            }

            translatedKeyCodes[i] = keycode;
        }
    }
        
    if (virAsprintf(&selector, 
                    "CreationClassName=Msvm_Keyboard&DeviceID=%s&"
                    "SystemCreationClassName=Msvm_ComputerSystem&SystemName=%s",
                    keyboard->data->DeviceID, uuid_string) < 0)
        goto cleanup;

    /* Press keys */
    for (i = 0; i < nkeycodes; i++) {
        VIR_FREE(params);
        nb_params = 1;

        if (VIR_ALLOC_N(params, nb_params) < 0)
            goto cleanup;

        char keyCodeStr[sizeof(int)*3+2];
        snprintf(keyCodeStr, sizeof keyCodeStr, "%d", translatedKeyCodes[i]);

		simpleparam.value = keyCodeStr;

        (*params).name = "keyCode";
        (*params).type = SIMPLE_PARAM;
        (*params).param = &simpleparam;

        if (hypervInvokeMethod(priv, params, nb_params, "PressKey",
                               MSVM_KEYBOARD_2012_RESOURCE_URI, selector) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not press key with code %d"),
                           translatedKeyCodes[i]);
            goto cleanup;
        }
    }

    /* Hold keys (copied from vbox driver); since Hyper-V does not support
	   holdtime, simulate it by sleeping and then sending the release keys */
    if (holdtime > 0)
        usleep(holdtime * 1000);

    /* Release keys */
    for (i = 0; i < nkeycodes; i++) {
        VIR_FREE(params);
        nb_params = 1;

        if (VIR_ALLOC_N(params, nb_params) < 0)
            goto cleanup;

        char keyCodeStr[sizeof(int)*3+2];
        snprintf(keyCodeStr, sizeof keyCodeStr, "%d", translatedKeyCodes[i]);

		simpleparam.value = keyCodeStr;

        (*params).name = "keyCode";
        (*params).type = SIMPLE_PARAM;
        (*params).param = &simpleparam;

        if (hypervInvokeMethod(priv, params, nb_params, "ReleaseKey",
                               MSVM_KEYBOARD_2012_RESOURCE_URI, selector) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not release key with code %d"),
                           translatedKeyCodes[i]);
            goto cleanup;
        }
    }

    result = 0;

    cleanup:
        VIR_FREE(params);
        hypervFreeObject(priv, (hypervObject *) computerSystem);
        hypervFreeObject(priv, (hypervObject *) keyboard);
        virBufferFreeAndReset(&query);
        return result;
}

