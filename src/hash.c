/*
 * hash.c: chained hash tables for domain and domain/connection deallocations
 *
 * Reference: Your favorite introductory book on algorithms
 *
 * Copyright (C) 2000 Bjorn Reese and Daniel Veillard.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS AND
 * CONTRIBUTORS ACCEPT NO RESPONSIBILITY IN ANY CONCEIVABLE MANNER.
 *
 * Author: breese@users.sourceforge.net
 *         Daniel Veillard <veillard@redhat.com>
 */

#include <string.h>
#include <stdlib.h>
#include <libxml/threads.h>
#include "internal.h"
#include "hash.h"

#define MAX_HASH_LEN 8

/* #define DEBUG_GROW */

/*
 * A single entry in the hash table
 */
typedef struct _virHashEntry virHashEntry;
typedef virHashEntry *virHashEntryPtr;
struct _virHashEntry {
    struct _virHashEntry *next;
    char *name;
    void *payload;
    int valid;
};

/*
 * The entire hash table
 */
struct _virHashTable {
    struct _virHashEntry *table;
    int size;
    int nbElems;
};

/*
 * virHashComputeKey:
 * Calculate the hash key
 */
static unsigned long
virHashComputeKey(virHashTablePtr table, const char *name)
{
    unsigned long value = 0L;
    char ch;

    if (name != NULL) {
        value += 30 * (*name);
        while ((ch = *name++) != 0) {
            value =
                value ^ ((value << 5) + (value >> 3) + (unsigned long) ch);
        }
    }
    return (value % table->size);
}

/**
 * virHashCreate:
 * @size: the size of the hash table
 *
 * Create a new virHashTablePtr.
 *
 * Returns the newly created object, or NULL if an error occured.
 */
virHashTablePtr
virHashCreate(int size)
{
    virHashTablePtr table;

    if (size <= 0)
        size = 256;

    table = malloc(sizeof(virHashTable));
    if (table) {
        table->size = size;
        table->nbElems = 0;
        table->table = calloc(1, size * sizeof(virHashEntry));
        if (table->table) {
            return (table);
        }
        free(table);
    }
    return (NULL);
}

/**
 * virHashGrow:
 * @table: the hash table
 * @size: the new size of the hash table
 *
 * resize the hash table
 *
 * Returns 0 in case of success, -1 in case of failure
 */
static int
virHashGrow(virHashTablePtr table, int size)
{
    unsigned long key;
    int oldsize, i;
    virHashEntryPtr iter, next;
    struct _virHashEntry *oldtable;

#ifdef DEBUG_GROW
    unsigned long nbElem = 0;
#endif

    if (table == NULL)
        return (-1);
    if (size < 8)
        return (-1);
    if (size > 8 * 2048)
        return (-1);

    oldsize = table->size;
    oldtable = table->table;
    if (oldtable == NULL)
        return (-1);

    table->table = calloc(1, size * sizeof(virHashEntry));
    if (table->table == NULL) {
        table->table = oldtable;
        return (-1);
    }
    table->size = size;

    /*  If the two loops are merged, there would be situations where
     * a new entry needs to allocated and data copied into it from 
     * the main table. So instead, we run through the array twice, first
     * copying all the elements in the main array (where we can't get
     * conflicts) and then the rest, so we only free (and don't allocate)
     */
    for (i = 0; i < oldsize; i++) {
        if (oldtable[i].valid == 0)
            continue;
        key = virHashComputeKey(table, oldtable[i].name);
        memcpy(&(table->table[key]), &(oldtable[i]), sizeof(virHashEntry));
        table->table[key].next = NULL;
    }

    for (i = 0; i < oldsize; i++) {
        iter = oldtable[i].next;
        while (iter) {
            next = iter->next;

            /*
             * put back the entry in the new table
             */

            key = virHashComputeKey(table, iter->name);
            if (table->table[key].valid == 0) {
                memcpy(&(table->table[key]), iter, sizeof(virHashEntry));
                table->table[key].next = NULL;
                free(iter);
            } else {
                iter->next = table->table[key].next;
                table->table[key].next = iter;
            }

#ifdef DEBUG_GROW
            nbElem++;
#endif

            iter = next;
        }
    }

    free(oldtable);

#ifdef DEBUG_GROW
    xmlGenericError(xmlGenericErrorContext,
                    "virHashGrow : from %d to %d, %d elems\n", oldsize,
                    size, nbElem);
#endif

    return (0);
}

/**
 * virHashFree:
 * @table: the hash table
 * @f:  the deallocator function for items in the hash
 *
 * Free the hash @table and its contents. The userdata is
 * deallocated with @f if provided.
 */
void
virHashFree(virHashTablePtr table, virHashDeallocator f)
{
    int i;
    virHashEntryPtr iter;
    virHashEntryPtr next;
    int inside_table = 0;
    int nbElems;

    if (table == NULL)
        return;
    if (table->table) {
        nbElems = table->nbElems;
        for (i = 0; (i < table->size) && (nbElems > 0); i++) {
            iter = &(table->table[i]);
            if (iter->valid == 0)
                continue;
            inside_table = 1;
            while (iter) {
                next = iter->next;
                if ((f != NULL) && (iter->payload != NULL))
                    f(iter->payload, iter->name);
                if (iter->name)
                    free(iter->name);
                iter->payload = NULL;
                if (!inside_table)
                    free(iter);
                nbElems--;
                inside_table = 0;
                iter = next;
            }
            inside_table = 0;
        }
        free(table->table);
    }
    free(table);
}

/**
 * virHashAddEntry3:
 * @table: the hash table
 * @name: the name of the userdata
 * @userdata: a pointer to the userdata
 *
 * Add the @userdata to the hash @table. This can later be retrieved
 * by using @name. Duplicate entries generate errors.
 *
 * Returns 0 the addition succeeded and -1 in case of error.
 */
int
virHashAddEntry(virHashTablePtr table, const char *name, void *userdata)
{
    unsigned long key, len = 0;
    virHashEntryPtr entry;
    virHashEntryPtr insert;

    if ((table == NULL) || (name == NULL))
        return (-1);

    /*
     * Check for duplicate and insertion location.
     */
    key = virHashComputeKey(table, name);
    if (table->table[key].valid == 0) {
        insert = NULL;
    } else {
        for (insert = &(table->table[key]); insert->next != NULL;
             insert = insert->next) {
            if (!strcmp(insert->name, name))
                return (-1);
            len++;
        }
        if (!strcmp(insert->name, name))
            return (-1);
    }

    if (insert == NULL) {
        entry = &(table->table[key]);
    } else {
        entry = malloc(sizeof(virHashEntry));
        if (entry == NULL)
            return (-1);
    }

    entry->name = strdup(name);
    entry->payload = userdata;
    entry->next = NULL;
    entry->valid = 1;


    if (insert != NULL)
        insert->next = entry;

    table->nbElems++;

    if (len > MAX_HASH_LEN)
        virHashGrow(table, MAX_HASH_LEN * table->size);

    return (0);
}

/**
 * virHashUpdateEntry:
 * @table: the hash table
 * @name: the name of the userdata
 * @userdata: a pointer to the userdata
 * @f: the deallocator function for replaced item (if any)
 *
 * Add the @userdata to the hash @table. This can later be retrieved
 * by using @name. Existing entry for this tuple
 * will be removed and freed with @f if found.
 *
 * Returns 0 the addition succeeded and -1 in case of error.
 */
int
virHashUpdateEntry(virHashTablePtr table, const char *name,
                   void *userdata, virHashDeallocator f)
{
    unsigned long key;
    virHashEntryPtr entry;
    virHashEntryPtr insert;

    if ((table == NULL) || name == NULL)
        return (-1);

    /*
     * Check for duplicate and insertion location.
     */
    key = virHashComputeKey(table, name);
    if (table->table[key].valid == 0) {
        insert = NULL;
    } else {
        for (insert = &(table->table[key]); insert->next != NULL;
             insert = insert->next) {
            if (!strcmp(insert->name, name)) {
                if (f)
                    f(insert->payload, insert->name);
                insert->payload = userdata;
                return (0);
            }
        }
        if (!strcmp(insert->name, name)) {
            if (f)
                f(insert->payload, insert->name);
            insert->payload = userdata;
            return (0);
        }
    }

    if (insert == NULL) {
        entry = &(table->table[key]);
    } else {
        entry = malloc(sizeof(virHashEntry));
        if (entry == NULL)
            return (-1);
    }

    entry->name = strdup(name);
    entry->payload = userdata;
    entry->next = NULL;
    entry->valid = 1;
    table->nbElems++;


    if (insert != NULL) {
        insert->next = entry;
    }
    return (0);
}

/**
 * virHashLookup:
 * @table: the hash table
 * @name: the name of the userdata
 *
 * Find the userdata specified by the (@name, @name2, @name3) tuple.
 *
 * Returns the a pointer to the userdata
 */
void *
virHashLookup(virHashTablePtr table, const char *name)
{
    unsigned long key;
    virHashEntryPtr entry;

    if (table == NULL)
        return (NULL);
    if (name == NULL)
        return (NULL);
    key = virHashComputeKey(table, name);
    if (table->table[key].valid == 0)
        return (NULL);
    for (entry = &(table->table[key]); entry != NULL; entry = entry->next) {
        if (!strcmp(entry->name, name))
            return (entry->payload);
    }
    return (NULL);
}

/**
 * virHashSize:
 * @table: the hash table
 *
 * Query the number of elements installed in the hash @table.
 *
 * Returns the number of elements in the hash table or
 * -1 in case of error
 */
int
virHashSize(virHashTablePtr table)
{
    if (table == NULL)
        return (-1);
    return (table->nbElems);
}

/**
 * virHashRemoveEntry:
 * @table: the hash table
 * @name: the name of the userdata
 * @f: the deallocator function for removed item (if any)
 *
 * Find the userdata specified by the @name and remove
 * it from the hash @table. Existing userdata for this tuple will be removed
 * and freed with @f.
 *
 * Returns 0 if the removal succeeded and -1 in case of error or not found.
 */
int
virHashRemoveEntry(virHashTablePtr table, const char *name,
                   virHashDeallocator f)
{
    unsigned long key;
    virHashEntryPtr entry;
    virHashEntryPtr prev = NULL;

    if (table == NULL || name == NULL)
        return (-1);

    key = virHashComputeKey(table, name);
    if (table->table[key].valid == 0) {
        return (-1);
    } else {
        for (entry = &(table->table[key]); entry != NULL;
             entry = entry->next) {
            if (!strcmp(entry->name, name)) {
                if ((f != NULL) && (entry->payload != NULL))
                    f(entry->payload, entry->name);
                entry->payload = NULL;
                if (entry->name)
                    free(entry->name);
                if (prev) {
                    prev->next = entry->next;
                    free(entry);
                } else {
                    if (entry->next == NULL) {
                        entry->valid = 0;
                    } else {
                        entry = entry->next;
                        memcpy(&(table->table[key]), entry,
                               sizeof(virHashEntry));
                        free(entry);
                    }
                }
                table->nbElems--;
                return (0);
            }
            prev = entry;
        }
        return (-1);
    }
}


/**
 * virHashForEach
 * @table: the hash table to process
 * @iter: callback to process each element
 * @data: opaque data to pass to the iterator
 *
 * Iterates over every element in the hash table, invoking the
 * 'iter' callback. The callback must not call any other virHash*
 * functions, and in particular must not attempt to remove the
 * element.
 *
 * Returns number of items iterated over upon completion, -1 on failure
 */
int virHashForEach(virHashTablePtr table, virHashIterator iter, const void *data) {
    int i, count = 0;

    if (table == NULL || iter == NULL)
        return (-1);

    for (i = 0 ; i < table->size ; i++) {
        virHashEntryPtr entry = table->table + i;
        while (entry) {
            if (entry->valid) {
                iter(entry->payload, entry->name, data);
                count++;
            }
            entry = entry->next;
        }
    }
    return (count);
}

/**
 * virHashRemoveSet
 * @table: the hash table to process
 * @iter: callback to identify elements for removal
 * @f: callback to free memory from element payload
 * @data: opaque data to pass to the iterator
 *
 * Iterates over all elements in the hash table, invoking the 'iter'
 * callback. If the callback returns a non-zero value, the element
 * will be removed from the hash table & its payload passed to the
 * callback 'f' for de-allocation.
 *
 * Returns number of items removed on success, -1 on failure
 */
int virHashRemoveSet(virHashTablePtr table, virHashSearcher iter, virHashDeallocator f, const void *data) {
    int i, count = 0;

    if (table == NULL || iter == NULL)
        return (-1);

    for (i = 0 ; i < table->size ; i++) {
        virHashEntryPtr prev = NULL;
        virHashEntryPtr entry = &(table->table[i]);

        while (entry && entry->valid) {
            if (iter(entry->payload, entry->name, data)) {
                count++;
                f(entry->payload, entry->name);
                if (entry->name)
                    free(entry->name);
                if (prev) {
                    prev->next = entry->next;
                    free(entry);
                } else {
                    if (entry->next == NULL) {
                        entry->valid = 0;
                        entry->name = NULL;
                    } else {
                        entry = entry->next;
                        memcpy(&(table->table[i]), entry,
                               sizeof(virHashEntry));
                        free(entry);
                        entry = NULL;
                    }
                }
                table->nbElems--;
            }
            prev = entry;
            if (entry) {
                entry = entry->next;
            } else {
                entry = NULL;
            }
        }
    }
    return (count);
}

/**
 * virHashSearch:
 * @table: the hash table to search
 * @iter: an iterator to identify the desired element
 * @data: extra opaque information passed to the iter
 *
 * Iterates over the hash table calling the 'iter' callback
 * for each element. The first element for which the iter
 * returns non-zero will be returned by this function.
 * The elements are processed in a undefined order
 */
void *virHashSearch(virHashTablePtr table, virHashSearcher iter, const void *data) {
    int i;

    if (table == NULL || iter == NULL)
        return (NULL);

    for (i = 0 ; i < table->size ; i++) {
        virHashEntryPtr entry = table->table + i;
        while (entry) {
            if (entry->valid) {
                if (iter(entry->payload, entry->name, data))
                    return entry->payload;
            }
            entry = entry->next;
        }
    }
    return (NULL);
}

/************************************************************************
 *									*
 *			Domain and Connections allocations		*
 *									*
 ************************************************************************/

/**
 * virHashError:
 * @conn: the connection if available
 * @error: the error noumber
 * @info: extra information string
 *
 * Handle an error at the connection level
 */
static void
virHashError(virConnectPtr conn, virErrorNumber error, const char *info)
{
    const char *errmsg;

    if (error == VIR_ERR_OK)
        return;

    errmsg = __virErrorMsg(error, info);
    __virRaiseError(conn, NULL, NULL, VIR_FROM_NONE, error, VIR_ERR_ERROR,
                    errmsg, info, NULL, 0, 0, errmsg, info);
}


/**
 * virDomainFreeName:
 * @domain: a domain object
 *
 * Destroy the domain object, this is just used by the domain hash callback.
 *
 * Returns 0 in case of success and -1 in case of failure.
 */
static int
virDomainFreeName(virDomainPtr domain, const char *name ATTRIBUTE_UNUSED)
{
    return (virDomainFree(domain));
}

/**
 * virNetworkFreeName:
 * @network: a network object
 *
 * Destroy the network object, this is just used by the network hash callback.
 *
 * Returns 0 in case of success and -1 in case of failure.
 */
static int
virNetworkFreeName(virNetworkPtr network, const char *name ATTRIBUTE_UNUSED)
{
    return (virNetworkFree(network));
}

/**
 * virGetConnect:
 *
 * Allocates a new hypervisor connection structure
 *
 * Returns a new pointer or NULL in case of error.
 */
virConnectPtr
virGetConnect(void) {
    virConnectPtr ret;

    ret = (virConnectPtr) calloc(1, sizeof(virConnect));
    if (ret == NULL) {
        virHashError(NULL, VIR_ERR_NO_MEMORY, _("allocating connection"));
        goto failed;
    }
    ret->magic = VIR_CONNECT_MAGIC;
    ret->driver = NULL;
    ret->networkDriver = NULL;
    ret->privateData = NULL;
    ret->networkPrivateData = NULL;
    ret->domains = virHashCreate(20);
    if (ret->domains == NULL)
        goto failed;
    ret->networks = virHashCreate(20);
    if (ret->networks == NULL)
        goto failed;
    ret->hashes_mux = xmlNewMutex();
    if (ret->hashes_mux == NULL)
        goto failed;

    ret->uses = 1;
    return(ret);

failed:
    if (ret != NULL) {
	if (ret->domains != NULL)
	    virHashFree(ret->domains, (virHashDeallocator) virDomainFreeName);
	if (ret->networks != NULL)
	    virHashFree(ret->networks, (virHashDeallocator) virNetworkFreeName);
	if (ret->hashes_mux != NULL)
	    xmlFreeMutex(ret->hashes_mux);
        free(ret);
    }
    return(NULL);
}

/**
 * virFreeConnect:
 * @conn: the hypervisor connection
 *
 * Release the connection. if the use count drops to zero, the structure is
 * actually freed.
 *
 * Returns the reference count or -1 in case of failure.
 */
int	
virFreeConnect(virConnectPtr conn) {
    int ret;

    if ((!VIR_IS_CONNECT(conn)) || (conn->hashes_mux == NULL)) {
        virHashError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }
    xmlMutexLock(conn->hashes_mux);
    conn->uses--;
    ret = conn->uses;
    if (ret > 0) {
	xmlMutexUnlock(conn->hashes_mux);
	return(ret);
    }

    if (conn->domains != NULL)
        virHashFree(conn->domains, (virHashDeallocator) virDomainFreeName);
    if (conn->networks != NULL)
        virHashFree(conn->networks, (virHashDeallocator) virNetworkFreeName);
    if (conn->hashes_mux != NULL)
        xmlFreeMutex(conn->hashes_mux);
    virResetError(&conn->err);
    free(conn);
    return(0);
}

/**
 * virGetDomain:
 * @conn: the hypervisor connection
 * @name: pointer to the domain name
 * @uuid: pointer to the uuid
 *
 * Lookup if the domain is already registered for that connection,
 * if yes return a new pointer to it, if no allocate a new structure,
 * and register it in the table. In any case a corresponding call to
 * virFreeDomain() is needed to not leak data.
 *
 * Returns a pointer to the domain, or NULL in case of failure
 */
virDomainPtr
__virGetDomain(virConnectPtr conn, const char *name, const unsigned char *uuid) {
    virDomainPtr ret = NULL;

    if ((!VIR_IS_CONNECT(conn)) || (name == NULL) || (uuid == NULL) ||
        (conn->hashes_mux == NULL)) {
        virHashError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(NULL);
    }
    xmlMutexLock(conn->hashes_mux);

    /* TODO search by UUID first as they are better differenciators */

    ret = (virDomainPtr) virHashLookup(conn->domains, name);
    if (ret != NULL) {
        /* TODO check the UUID */
	goto done;
    }

    /*
     * not found, allocate a new one
     */
    ret = (virDomainPtr) calloc(1, sizeof(virDomain));
    if (ret == NULL) {
        virHashError(conn, VIR_ERR_NO_MEMORY, _("allocating domain"));
	goto error;
    }
    ret->name = strdup(name);
    if (ret->name == NULL) {
        virHashError(conn, VIR_ERR_NO_MEMORY, _("allocating domain"));
	goto error;
    }
    ret->magic = VIR_DOMAIN_MAGIC;
    ret->conn = conn;
    ret->id = -1;
    if (uuid != NULL)
        memcpy(&(ret->uuid[0]), uuid, VIR_UUID_BUFLEN);

    if (virHashAddEntry(conn->domains, name, ret) < 0) {
        virHashError(conn, VIR_ERR_INTERNAL_ERROR,
	             _("failed to add domain to connection hash table"));
	goto error;
    }
    conn->uses++;
done:
    ret->uses++;
    xmlMutexUnlock(conn->hashes_mux);
    return(ret);

error:
    xmlMutexUnlock(conn->hashes_mux);
    if (ret != NULL) {
	if (ret->name != NULL)
	    free(ret->name );
	free(ret);
    }
    return(NULL);
}

/**
 * virFreeDomain:
 * @conn: the hypervisor connection
 * @domain: the domain to release
 *
 * Release the given domain, if the reference count drops to zero, then
 * the domain is really freed.
 *
 * Returns the reference count or -1 in case of failure.
 */
int
virFreeDomain(virConnectPtr conn, virDomainPtr domain) {
    int ret = 0;

    if ((!VIR_IS_CONNECT(conn)) || (!VIR_IS_CONNECTED_DOMAIN(domain)) ||
        (domain->conn != conn) || (conn->hashes_mux == NULL)) {
        virHashError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }
    xmlMutexLock(conn->hashes_mux);

    /*
     * decrement the count for the domain
     */
    domain->uses--;
    ret = domain->uses;
    if (ret > 0)
        goto done;

    /* TODO search by UUID first as they are better differenciators */

    if (virHashRemoveEntry(conn->domains, domain->name, NULL) < 0) {
        virHashError(conn, VIR_ERR_INTERNAL_ERROR,
	             _("domain missing from connection hash table"));
        goto done;
    }
    domain->magic = -1;
    domain->id = -1;
    if (domain->name)
        free(domain->name);
    free(domain);

    /*
     * decrement the count for the connection
     */
    conn->uses--;
    if (conn->uses > 0)
        goto done;
    
    if (conn->domains != NULL)
        virHashFree(conn->domains, (virHashDeallocator) virDomainFreeName);
    if (conn->hashes_mux != NULL)
        xmlFreeMutex(conn->hashes_mux);
    free(conn);
    return(0);

done:
    xmlMutexUnlock(conn->hashes_mux);
    return(ret);
}

/**
 * virGetNetwork:
 * @conn: the hypervisor connection
 * @name: pointer to the network name
 * @uuid: pointer to the uuid
 *
 * Lookup if the network is already registered for that connection,
 * if yes return a new pointer to it, if no allocate a new structure,
 * and register it in the table. In any case a corresponding call to
 * virFreeNetwork() is needed to not leak data.
 *
 * Returns a pointer to the network, or NULL in case of failure
 */
virNetworkPtr
__virGetNetwork(virConnectPtr conn, const char *name, const unsigned char *uuid) {
    virNetworkPtr ret = NULL;

    if ((!VIR_IS_CONNECT(conn)) || (name == NULL) || (uuid == NULL) ||
        (conn->hashes_mux == NULL)) {
        virHashError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(NULL);
    }
    xmlMutexLock(conn->hashes_mux);

    /* TODO search by UUID first as they are better differenciators */

    ret = (virNetworkPtr) virHashLookup(conn->networks, name);
    if (ret != NULL) {
        /* TODO check the UUID */
	goto done;
    }

    /*
     * not found, allocate a new one
     */
    ret = (virNetworkPtr) calloc(1, sizeof(virNetwork));
    if (ret == NULL) {
        virHashError(conn, VIR_ERR_NO_MEMORY, _("allocating network"));
	goto error;
    }
    ret->name = strdup(name);
    if (ret->name == NULL) {
        virHashError(conn, VIR_ERR_NO_MEMORY, _("allocating network"));
	goto error;
    }
    ret->magic = VIR_NETWORK_MAGIC;
    ret->conn = conn;
    if (uuid != NULL)
        memcpy(&(ret->uuid[0]), uuid, VIR_UUID_BUFLEN);

    if (virHashAddEntry(conn->networks, name, ret) < 0) {
        virHashError(conn, VIR_ERR_INTERNAL_ERROR,
	             _("failed to add network to connection hash table"));
	goto error;
    }
    conn->uses++;
done:
    ret->uses++;
    xmlMutexUnlock(conn->hashes_mux);
    return(ret);

error:
    xmlMutexUnlock(conn->hashes_mux);
    if (ret != NULL) {
	if (ret->name != NULL)
	    free(ret->name );
	free(ret);
    }
    return(NULL);
}

/**
 * virFreeNetwork:
 * @conn: the hypervisor connection
 * @network: the network to release
 *
 * Release the given network, if the reference count drops to zero, then
 * the network is really freed.
 *
 * Returns the reference count or -1 in case of failure.
 */
int
virFreeNetwork(virConnectPtr conn, virNetworkPtr network) {
    int ret = 0;

    if ((!VIR_IS_CONNECT(conn)) || (!VIR_IS_CONNECTED_NETWORK(network)) ||
        (network->conn != conn) || (conn->hashes_mux == NULL)) {
        virHashError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(-1);
    }
    xmlMutexLock(conn->hashes_mux);

    /*
     * decrement the count for the network
     */
    network->uses--;
    ret = network->uses;
    if (ret > 0)
        goto done;

    /* TODO search by UUID first as they are better differenciators */

    if (virHashRemoveEntry(conn->networks, network->name, NULL) < 0) {
        virHashError(conn, VIR_ERR_INTERNAL_ERROR,
	             _("network missing from connection hash table"));
        goto done;
    }
    network->magic = -1;
    if (network->name)
        free(network->name);
    free(network);

    /*
     * decrement the count for the connection
     */
    conn->uses--;
    if (conn->uses > 0)
        goto done;

    if (conn->networks != NULL)
        virHashFree(conn->networks, (virHashDeallocator) virNetworkFreeName);
    if (conn->hashes_mux != NULL)
        xmlFreeMutex(conn->hashes_mux);
    free(conn);
    return(0);

done:
    xmlMutexUnlock(conn->hashes_mux);
    return(ret);
}

/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
