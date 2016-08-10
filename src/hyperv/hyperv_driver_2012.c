#include "hyperv_driver_2012.h"

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
               (domain, MSVM_COMPUTERSYSTEM_REQUESTEDSTATE_ENABLED);

 cleanup:
    hypervFreeObject(priv, (hypervObject *)computerSystem);

    return result;
}

int
hypervDomainCreate2012(virDomainPtr domain)
{
    return hypervDomainCreateWithFlags2012(domain, 0);
}
