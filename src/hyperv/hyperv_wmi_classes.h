/*
 * hyperv_wmi_classes.h: WMI classes for managing Microsoft Hyper-V hosts
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

#ifndef __HYPERV_WMI_CLASSES_H__
# define __HYPERV_WMI_CLASSES_H__

# include "openwsman.h"

# include "hyperv_wmi_classes.generated.typedef"



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Msvm_ComputerSystem
 */

# define MSVM_COMPUTERSYSTEM_V1_WQL_VIRTUAL \
    "Description = \"Microsoft Virtual Machine\" "

# define MSVM_COMPUTERSYSTEM_V1_WQL_PHYSICAL \
    "Description = \"Microsoft Hosting Computer System\" "

# define MSVM_COMPUTERSYSTEM_V1_WQL_ACTIVE \
    "(EnabledState != 0 and EnabledState != 3 and EnabledState != 32769) "

# define MSVM_COMPUTERSYSTEM_V1_WQL_INACTIVE \
    "(EnabledState = 0 or EnabledState = 3 or EnabledState = 32769) "

enum _Msvm_ComputerSystem_v1_EnabledState {
    MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_UNKNOWN = 0,          /* inactive */
    MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_ENABLED = 2,          /*   active */
    MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_DISABLED = 3,         /* inactive */
    MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_PAUSED = 32768,       /*   active */
    MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SUSPENDED = 32769,    /* inactive */
    MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_STARTING = 32770,     /*   active */
    MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SNAPSHOTTING = 32771, /*   active */
    MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_SAVING = 32773,       /*   active */
    MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_STOPPING = 32774,     /*   active */
    MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_PAUSING = 32776,      /*   active */
    MSVM_COMPUTERSYSTEM_V1_ENABLEDSTATE_RESUMING = 32777      /*   active */
};

enum _Msvm_ComputerSystem_v1_RequestedState {
    MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_ENABLED = 2,
    MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_DISABLED = 3,
    MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_REBOOT = 10,
    MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_PAUSED = 32768,
    MSVM_COMPUTERSYSTEM_V1_REQUESTEDSTATE_SUSPENDED = 32769,
};

enum _Msvm_ReturnCode {
    MSVM_RETURNCODE_V1_FAILED = 32768,
    MSVM_RETURNCODE_V1_ACCESS_DENIED = 32769,
    MSVM_RETURNCODE_V1_NOT_SUPPORTED = 32770,
    MSVM_RETURNCODE_V1_STATUS_IS_UNKNOWN = 32771,
    MSVM_RETURNCODE_V1_TIMEOUT = 32772,
    MSVM_RETURNCODE_V1_INVALID_PARAMETER = 32773,
    MSVM_RETURNCODE_V1_SYSTEM_IS_IN_USE = 32774,
    MSVM_RETURNCODE_V1_INVALID_STATE_FOR_THIS_OPERATION = 32775,
    MSVM_RETURNCODE_V1_INCORRECT_DATA_TYPE = 32776,
    MSVM_RETURNCODE_V1_SYSTEM_IS_NOT_AVAILABLE = 32777,
    MSVM_RETURNCODE_V1_OUT_OF_MEMORY = 32778,
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * CIM/Msvm_ReturnCode
 */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Msvm_ConcreteJob
 */
enum _CIM_ReturnCode {
    CIM_RETURNCODE_COMPLETED_WITH_NO_ERROR = 0,
    CIM_RETURNCODE_NOT_SUPPORTED = 1,
    CIM_RETURNCODE_UNKNOWN_ERROR = 2,
    CIM_RETURNCODE_CANNOT_COMPLETE_WITHIN_TIMEOUT_PERIOD = 3,
    CIM_RETURNCODE_FAILED = 4,
    CIM_RETURNCODE_INVALID_PARAMETER = 5,
    CIM_RETURNCODE_IN_USE = 6,
    CIM_RETURNCODE_TRANSITION_STARTED = 4096,
    CIM_RETURNCODE_INVALID_STATE_TRANSITION = 4097,
    CIM_RETURNCODE_TIMEOUT_PARAMETER_NOT_SUPPORTED = 4098,
    CIM_RETURNCODE_BUSY = 4099,
};

enum _Msvm_ConcreteJob_JobState {
    MSVM_CONCRETEJOB_V1_JOBSTATE_NEW = 2,
    MSVM_CONCRETEJOB_V1_JOBSTATE_STARTING = 3,
    MSVM_CONCRETEJOB_V1_JOBSTATE_RUNNING = 4,
    MSVM_CONCRETEJOB_V1_JOBSTATE_SUSPENDED = 5,
    MSVM_CONCRETEJOB_V1_JOBSTATE_SHUTTING_DOWN = 6,
    MSVM_CONCRETEJOB_V1_JOBSTATE_COMPLETED = 7,
    MSVM_CONCRETEJOB_V1_JOBSTATE_TERMINATED = 8,
    MSVM_CONCRETEJOB_V1_JOBSTATE_KILLED = 9,
    MSVM_CONCRETEJOB_V1_JOBSTATE_EXCEPTION = 10,
    MSVM_CONCRETEJOB_V1_JOBSTATE_SERVICE = 11,
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Msvm_ResourceAllocationSettingData
 */

/* https://msdn.microsoft.com/en-us/library/cc136877(v=vs.85).aspx */
enum _Msvm_ResourceAllocationSettingData_ResourceType {
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_OTHER = 1,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_IDE_CONTROLLER = 5,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_PARALLEL_SCSI_HBA = 6,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_ETHERNET_ADAPTER = 10,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_FLOPPY = 14,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_CD_DRIVE = 15,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_DVD_DRIVE = 16,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_SERIAL_PORT = 17,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_STORAGE_EXTENT = 21,
    MSVM_RESOURCEALLOCATIONSETTINGDATA_V1_RESOURCETYPE_DISK = 22,
};


# include "hyperv_wmi_classes.generated.h"

#endif /* __HYPERV_WMI_CLASSES_H__ */
