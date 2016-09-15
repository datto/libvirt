#include "hyperv_driver_shared.h"

VIR_LOG_INIT("hyperv.hyperv_driver_shared");

/**
 * Find parent RASD entry from RASD list. This is done by walking 
 * through the entire device list and comparing the 'Parent' entry
 * of the disk RASD entry with the potential parent's 'InstanceID'.
 */  
int
hypervParseDomainDefFindParentRasd(
            Msvm_ResourceAllocationSettingData *rasdEntry,
            Msvm_ResourceAllocationSettingData *rasdEntryListStart,
            Msvm_ResourceAllocationSettingData **rasdEntryParent)
{
    int result = -1;
    char *expectedInstanceIdEndsWithStr;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_ResourceAllocationSettingData *rasdEntryArr = rasdEntryListStart;
    
    while (rasdEntryArr != NULL) {
        virBufferAsprintf(&query, "%s\"", rasdEntryArr->data->InstanceID);
        expectedInstanceIdEndsWithStr = virBufferContentAndReset(&query);                
        expectedInstanceIdEndsWithStr = virStringReplace(expectedInstanceIdEndsWithStr, "\\", "\\\\");
    
        if (virStringEndsWith(rasdEntry->data->Parent, expectedInstanceIdEndsWithStr)) {                
            *rasdEntryParent = rasdEntryArr;
            break;
        }            

        // Move to next item in linked list            
        rasdEntryArr = rasdEntryArr->next;
    }
    
    if (*rasdEntryParent != NULL) {    
        result = 0;
    }
    
    return result; 
}

/**
 * Converts a RASD entry to the 'dst' field in a disk definition (aka 
 * virDomainDiskDefPtr), i.e. maps the ISCSI / IDE controller index/address
 * and drive address/index to the guest drive name, e.g. sda, sdr, hda, hdb, ...
 *
 * WARNING, side effects:
 *   This function increases the SCSI drive count in the 'scsiDriveIndex' 
 *   parameter for every SCSI drive that is encountered. This is necessary
 *   because Hyper-V / WMI does NOT return an address for the SCSI drive.
 */
int
hypervParseDomainDefSetDiskTarget(
            virDomainDiskDefPtr disk,
            Msvm_ResourceAllocationSettingData *rasdEntry,
            Msvm_ResourceAllocationSettingData *rasdEntryListStart,
            int *scsiDriveIndex)
{
    int result = -1;
    char *expectedInstanceIdEndsWithStr;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    int ideControllerIndex = 0;
    int scsiControllerIndex = 0;    
    int driveIndex = 0;
    Msvm_ResourceAllocationSettingData *rasdEntryArr = rasdEntryListStart;

    /* Index of drive relative to controller. */
    if (rasdEntry->data->Address == NULL) {
        VIR_DEBUG("Drive does not have an address. Skipping.");
        goto cleanup;
    }

    driveIndex = atoi(rasdEntry->data->Address);
      
    /* Find parent IDE/SCSI controller in RASD list. This is done by walking 
     * through the entire device list and comparing the 'Parent' entry
     * of the disk RASD entry with the potential parent's 'InstanceID'.
     * 
     * Example:
     *   Disk RASD entry 'Parent': 
     *     \\WIN-S7J17Q4LBT7\root\virtualization:Msvm_ResourceAllocationSettingD
     *     ata.InstanceID="Microsoft:5E855AD2-5FD1-457E-A757-E48D7EC66072\\83F86
     *     38B-8DCA-4152-9EDA-2CA8B33039B4\\0"
     * 
     *   Matching parent RASD entry 'InstanceID':
     *     Microsoft:5E855AD2-5FD1-457E-A757-E48D7EC66072\83F8638B-8DCA-4152-9ED
     *     A-2CA8B33039B4\0
     */        
     
    while (rasdEntryArr != NULL) {
        virBufferAsprintf(&query, "%s\"", rasdEntryArr->data->InstanceID);
        expectedInstanceIdEndsWithStr = virBufferContentAndReset(&query);                
        expectedInstanceIdEndsWithStr = virStringReplace(expectedInstanceIdEndsWithStr, "\\", "\\\\");
    
        if (virStringEndsWith(rasdEntry->data->Parent, expectedInstanceIdEndsWithStr)) {                
            if (rasdEntryArr->data->ResourceType == MSVM_RESOURCEALLOCATIONSETTINGDATA_RESOURCETYPE_IDE_CONTROLLER) {                             
                ideControllerIndex = atoi(rasdEntryArr->data->Address);
                
                disk->bus = VIR_DOMAIN_DISK_BUS_IDE;
                disk->dst = virIndexToDiskName(ideControllerIndex * 2 + driveIndex, "hd"); // max. 2 drives per IDE bus
                break;
            } else if (rasdEntryArr->data->ResourceType == MSVM_RESOURCEALLOCATIONSETTINGDATA_RESOURCETYPE_PARALLEL_SCSI_HBA) {
                disk->bus = VIR_DOMAIN_DISK_BUS_SCSI;
                disk->dst = virIndexToDiskName(scsiControllerIndex * 15 + *scsiDriveIndex, "sd");
                
                (*scsiDriveIndex)++;
                break;
            }
        }            

        // Count SCSI controllers (IDE bus has 'Address' field)
        if (rasdEntryArr->data->ResourceType == MSVM_RESOURCEALLOCATIONSETTINGDATA_RESOURCETYPE_PARALLEL_SCSI_HBA) {
            scsiControllerIndex++;
        }

        // Move to next item in linked list            
        rasdEntryArr = rasdEntryArr->next;
    }
    
    result = 0;
    
  cleanup:    
    return result;    
}

/**
 * This parses the RASD entry for resource type 21 (Microsoft Virtual Hard Disk,
 * aka Hard Disk Image). This entry is used to represent VHD/ISO files that
 * are attached to a virtual drive.
 *
 * This implementation will find the parent virtual drive (type 22), and then
 * from there the IDE controller via the 'Parent' property, to fill the 
 * 'dst' (<target dev=..> field.
 *
 * RASD entry hierarchy
 * --------------------
 * IDE controller (type 5) or SCSI Controller (type 6)
 * `-- Hard Drive (type 22)
 *     `-- Hard Disk Image (type 21, with 'Connection' field)
 * 
 * Example RASD entries (shortened)
 * --------------------------------
 * instance of Msvm_ResourceAllocationSettingData
 * {
 *  Caption = "Hard Disk Image";
 *  Connection = {"E:\\somedisk.vhd"};
 *  ElementName = "Hard Disk Image";
 *  InstanceID = "Microsoft:5E855AD2-5FD1-457E-A757-E48D7EC66072\\B93006DA-38DE-
 *                4AE3-A847-9E094330C71F\\0\\1\\L";
 *  Parent = "\\\\WIN-S7J17Q4LBT7\\root\\virtualization:Msvm_ResourceAllocationS
 *            ettingData.InstanceID=\"Microsoft:5E855AD2-5FD1-457E-A757-E48D7EC6
 *            6072\\\\B93006DA-38DE-4AE3-A847-9E094330C71F\\\\0\\\\1\\\\D\"";
 *  ResourceType = 21;
 *  ...
 * };
 *
 * instance of Msvm_ResourceAllocationSettingData
 * {
 *  Address = "1";
 *  Caption = "Hard Drive";
 *  InstanceID = "Microsoft:5E855AD2-5FD1-457E-A757-E48D7EC66072\\B93006DA-38DE-
 *                4AE3-A847-9E094330C71F\\0\\1\\D";
 *  Parent = "\\\\WIN-S7J17Q4LBT7\\root\\virtualization:Msvm_ResourceAllocationS
 *            ettingData.InstanceID=\"Microsoft:5E855AD2-5FD1-457E-A757-E48D7EC6
 *            6072\\\\B93006DA-38DE-4AE3-A847-9E094330C71F\\\\0\"";
 *  ResourceType = 22;
 *  ...
 * };
 *  
 * instance of Msvm_ResourceAllocationSettingData
 * {
 *  Caption = "SCSI Controller";
 *  InstanceID = "Microsoft:5E855AD2-5FD1-457E-A757-E48D7EC66072\\B93006DA-38DE-
 *                4AE3-A847-9E094330C71F\\0";
 *  ResourceType = 6;
 *  ...
 * }
 */
int
hypervParseDomainDefStorageExtent(
            virDomainPtr domain, virDomainDefPtr def,
            Msvm_ResourceAllocationSettingData *rasdEntry,
            Msvm_ResourceAllocationSettingData *rasdEntryListStart,            
            int *scsiDriveIndex)
{
    int result = -1;
    char **connData;    
    hypervPrivate *priv = domain->conn->privateData;
    virDomainDiskDefPtr disk;
    Msvm_ResourceAllocationSettingData *hddOrDvdParentRasdEntry = NULL;    

    if (rasdEntry->data->Connection.count > 0) {
        VIR_DEBUG("Parsing device 'storage extent' (type %d)", 
                  rasdEntry->data->ResourceType);
    
        /* Define new disk */
        disk = virDomainDiskDefNew(priv->xmlopt);

        /* Find CD/DVD or HDD drive this entry is associated to */
        if (hypervParseDomainDefFindParentRasd(rasdEntry, rasdEntryListStart,
                                               &hddOrDvdParentRasdEntry) < 0) {
            VIR_DEBUG("Cannot find parent CD/DVD/HDD drive. Skipping.");
            goto cleanup;
        }    

        /* Target (dst and bus) */
        if (hypervParseDomainDefSetDiskTarget(disk, hddOrDvdParentRasdEntry,
            rasdEntryListStart, scsiDriveIndex) < 0) {
            VIR_DEBUG("Cannot set target. Skipping.");
            goto cleanup;
        }

        /* Type */
        virDomainDiskSetType(disk, VIR_STORAGE_TYPE_FILE);

        /* Source */
        connData = rasdEntry->data->Connection.data;

        if (virDomainDiskSetSource(disk, *connData) < 0) {
            VIR_FREE(connData);
            goto cleanup;
        }
        
        /* Device (CD/DVD or disk) */
        switch (hddOrDvdParentRasdEntry->data->ResourceType) {
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_RESOURCETYPE_CD_DRIVE:
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_RESOURCETYPE_DVD_DRIVE:
                disk->device = VIR_DOMAIN_DISK_DEVICE_CDROM;
                break;
                
            case MSVM_RESOURCEALLOCATIONSETTINGDATA_RESOURCETYPE_DISK:
            default:
                disk->device = VIR_DOMAIN_DISK_DEVICE_DISK;
                break;
        }            

        /* Bus */
        def->disks[def->ndisks] = disk;
        def->ndisks++;
    }

    result = 0;
    
    cleanup:    
        return result;       
}



/**
 * This parses the RASD entry for resource type 22 (Microsoft Synthetic Disk 
 * Drive, aka Hard Drive). For passthru disks, this entry has a 'HostResource'
 * property that points to the physical disk. If an ISO/VHD is mounted, 
 * this property is not present.
 *
 * This implementation will find the parent IDE controller via the 'Parent'
 * property, to fill the 'dst' (<target dev=..> field.
 *
 * RASD entry hierarchy
 * --------------------
 * IDE controller (type 5) or SCSI Controller (type 6)
 * `-- Hard Drive (type 22, with property 'HostResource')
 * 
 * Example RASD entries (shortened)
 * --------------------------------
 *
 * instance of Msvm_ResourceAllocationSettingData
 * {
 *  Address = "1";
 *  ElementName = "Hard Drive";
 *  HostResource = {"\\\\WIN-S7J17Q4LBT7\\root\\virtualization:Msvm_DiskDrive.Cr
 *                   eationClassName=\"Msvm_DiskDrive\",DeviceID=\"Microsoft:353
 *                   B3BE8-310C-4cf4-839E-4E1B14616136\\\\NODRIVE\",SystemCreati
 *                   onClassName=\"Msvm_ComputerSystem\",SystemName=\"WIN-S7J17Q
 *                   4LBT7\""};
 *  InstanceID = "Microsoft:5E855AD2-5FD1-457E-A757-E48D7EC66072\\83F8638B-8DCA-
 *                4152-9EDA-2CA8B33039B4\\1\\1\\D";
 *  Parent = "\\\\WIN-S7J17Q4LBT7\\root\\virtualization:Msvm_ResourceAllocationS
 *            ettingData.InstanceID=\"Microsoft:5E855AD2-5FD1-457E-A757-E48D7EC6
 *            6072\\\\83F8638B-8DCA-4152-9EDA-2CA8B33039B4\\\\1\"";
 *  ResourceType = 22;
 *  ...
 * };
 *
 * instance of Msvm_ResourceAllocationSettingData
 * {
 *  Address = "1";
 *  Caption = "IDE Controller 1";
 *  InstanceID = "Microsoft:5E855AD2-5FD1-457E-A757-E48D7EC66072\\83F8638B-8DCA-
 *                4152-9EDA-2CA8B33039B4\\1";
 *  ResourceType = 5;
 *  ...
 *  };
 */
int
hypervParseDomainDefDisk(
        virDomainPtr domain, virDomainDefPtr def,
        Msvm_ResourceAllocationSettingData *rasdEntry,
        Msvm_ResourceAllocationSettingData *rasdEntryListStart,
        int *scsiDriveIndex)
{
    int result = -1;
    char **hostResourceDataPath;
    char *hostResourceDataPathEscaped;    
    char driveNumberStr[11];    
    hypervPrivate *priv = domain->conn->privateData;
    virDomainDiskDefPtr disk;
    virBuffer query = VIR_BUFFER_INITIALIZER;
    Msvm_DiskDrive *diskDrive = NULL;

    /**
     * The 'HostResource' field contains the reference to the physical/virtual
     * disk (Msvm_DiskDrive) that this RASD entry points to. 
     * 
     * If this is empty, this drive is likely used as a virtual drive for 
     * ISO/VHD files, for which the logic is handled in the 
     * hypervParseDomainDefStorageExtent function.
     *
     * Example host resource entry:
     *    HostResource = {"\\\\WIN-S7J17Q4LBT7\\root\\virtualization:Msvm_DiskDr
     *    ive.CreationClassName=\"Msvm_DiskDrive\",DeviceID=\"Microsoft:353B3BE8
     *    -310C-4cf4-839E-4E1B14616136\\\\3\",SystemCreationClassName=\"Msvm_Com
     *    puterSystem\",SystemName=\"WIN-S7J17Q4LBT7\""};
     */
    if (rasdEntry->data->HostResource.count > 0) {  
        VIR_DEBUG("Parsing device 'disk' (type %d)", 
                  rasdEntry->data->ResourceType);
             
        /* Define new disk */
        disk = virDomainDiskDefNew(priv->xmlopt);        

        /* Escape HostResource path */        
        hostResourceDataPath = rasdEntry->data->HostResource.data;      
        hostResourceDataPathEscaped = virStringReplace(*hostResourceDataPath, "\\", "\\\\");
        hostResourceDataPathEscaped = virStringReplace(hostResourceDataPathEscaped, "\"", "\\\"");

        /* Get Msvm_DiskDrive (to get DriveNumber) */
        virBufferFreeAndReset(&query);
        virBufferAsprintf(&query,
                          "select * from Msvm_DiskDrive where __PATH=\"%s\"",
                          hostResourceDataPathEscaped);

        /* Please note:
         *     diskDrive could still be NULL, if no drive is attached,
         *     i.e. if "No disk selected" appears in the Hyper-V UI.
         */
        if (hypervGetMsvmDiskDriveList(priv, &query, &diskDrive) < 0) {
            goto cleanup;
        }        
        
        /* Target (dst and bus) */
        if (hypervParseDomainDefSetDiskTarget(disk, rasdEntry,
            rasdEntryListStart, scsiDriveIndex) < 0) {
            VIR_DEBUG("Cannot set target. Skipping.");
            goto cleanup;
        }        
        
        /* Type */
        virDomainDiskSetType(disk, VIR_STORAGE_TYPE_BLOCK);

        /* Source (Drive Number) */
        if (diskDrive != NULL && diskDrive->data != NULL) {            
            if (sprintf(driveNumberStr, "%d", diskDrive->data->DriveNumber) < 0) {
                goto cleanup;
            }
            
            if (virDomainDiskSetSource(disk, driveNumberStr) < 0) {
                VIR_FREE(driveNumberStr);
                goto cleanup;
            }
        } else {
            if (virDomainDiskSetSource(disk, "-1") < 0) { // No disk selected
                goto cleanup; 
            }
        }

        /* Add disk */
        def->disks[def->ndisks] = disk;
        def->ndisks++;
    }

    result = 0;

    cleanup:
        return result;
}

