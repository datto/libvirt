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

# define MSVM_COMPUTERSYSTEM_V2_WQL_VIRTUAL \
    "Description = \"Microsoft Virtual Machine\" "

# define MSVM_COMPUTERSYSTEM_V2_WQL_PHYSICAL \
    "Description = \"Microsoft Hosting Computer System\" "

# define MSVM_COMPUTERSYSTEM_V2_WQL_ACTIVE \
    "(EnabledState != 0 and EnabledState != 3 and EnabledState != 32769) "

# define MSVM_COMPUTERSYSTEM_V2_WQL_INACTIVE \
    "(EnabledState = 0 or EnabledState = 3 or EnabledState = 32769) "

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
 * CIM_ReturnCode
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

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Msvm_ResourceAllocationSettingData
 */



# include "hyperv_wmi_classes.generated.h"

#endif /* __HYPERV_WMI_CLASSES_H__ */
