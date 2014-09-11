/*
 * Apple System Management Controller (SMC) API from user space for Intel based
 * Macs. Works by talking to the AppleSMC.kext (kernel extension), the driver
 * for the SMC.
 *
 * smc.c
 * c-smc
 *
 * Copyright (C) 2014  beltex <https://github.com/beltex>
 *
 * Based off of fork from:
 * osx-cpu-temp <https://github.com/lavoiesl/osx-cpu-temp>
 *
 * With credits to:
 *
 * Copyright (C) 2006 devnull 
 * Apple System Management Control (SMC) Tool 
 *
 * Copyright (C) 2006 Hendrik Holtmann
 * smcFanControl <https://github.com/hholtmann/smcFanControl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <string.h>
#include <IOKit/IOKitLib.h>

#include "smc.h"


/**
Our connection to the SMC
*/
static io_connect_t conn;


//------------------------------------------------------------------------------
// MARK: HELPERS - TYPE CONVERSION
//------------------------------------------------------------------------------


/**
Convert data from SMC of fpe2 type to human readable.
    
:param: data Data from the SMC to be converted. Assumed data size of 2.
:returns: Converted data
*/
static unsigned int from_fpe2(uint8_t data[32])
{
    unsigned int ans = 0;
    
    // Data type for fan calls - fpe2
    // This is assumend to mean floating point, with 2 exponent bits
    // http://stackoverflow.com/questions/22160746/fpe2-and-sp78-data-types
    ans += data[0] << 6;
    ans += data[1] << 2;

    return ans;
}


/**
Convert to fpe2 data type to be passed to SMC.

:param: val Value to convert
:param: data Pointer to data array to place result
*/
static void to_fpe2(unsigned int val, uint8_t *data)
{
    data[0] = val >> 6;
    data[1] = (val << 2) ^ (data[0] << 8); 
}


/**
Convert SMC key to uint32_t. This must be done to pass it to the SMC.
    
:param: key The SMC key to convert
:returns: uint32_t translation.
          Returns zero if key is not 4 characters in length.
*/
static uint32_t to_uint32_t(char *key)
{
    uint32_t ans   = 0;
    uint32_t shift = 24;

    // SMC key is expected to be 4 bytes - thus 4 chars
    if (strlen(key) != SMC_KEY_SIZE) {
        return 0;
    }

    for (int i = 0; i < SMC_KEY_SIZE; i++) {
        ans += key[i] << shift;
        shift -= 8;
    }

    return ans;
}


/**
For converting the dataType return from the SMC to human readable 4 byte
multi-character constant.
*/
static void to_string(uint32_t val, char *dataType)
{
    int shift = 24;

    for (int i = 0; i < DATA_TYPE_SIZE; i++) {
        // To get each char, we shift it into the lower 8 bits, and then & by
        // 255 to insolate it
        dataType[i] = (val >> shift) & 0xff;
        shift -= 8;
    }
}


//------------------------------------------------------------------------------
// MARK: HELPERS - TMP CONVERSION
//------------------------------------------------------------------------------


/**
Celsius to Fahrenheit
*/
static double to_fahrenheit(double tmp)
{
    // http://en.wikipedia.org/wiki/Fahrenheit#Definition_and_conversions
    return (tmp * 1.8) + 32;
}


/**
Celsius to Kelvin
*/
static double to_kelvin(double tmp)
{
    // http://en.wikipedia.org/wiki/Kelvin
    return tmp + 273.15;
}


//------------------------------------------------------------------------------
// MARK: "PRIVATE" FUNCTIONS
//------------------------------------------------------------------------------


/**
Make a call to the SMC
    
:param: inputStruct Struct that holds data telling the SMC what you want
:param: outputStruct Struct holding the SMC's response
:returns: I/O Kit return code
*/
static kern_return_t call_smc(SMCParamStruct *inputStruct,
                              SMCParamStruct *outputStruct)
{
    kern_return_t result;
    size_t inputStructCnt  = sizeof(SMCParamStruct);
    size_t outputStructCnt = sizeof(SMCParamStruct);

    result = IOConnectCallStructMethod(conn, kSMCHandleYPCEvent,
                                             inputStruct,
                                             inputStructCnt,
                                             outputStruct,
                                             &outputStructCnt);
    
    if (result != kIOReturnSuccess) {
        // IOReturn error code lookup. See "Accessing Hardware From Applications
        // -> Handling Errors" Apple doc
        result = err_get_code(result);
    }

    return result;
}


/**
Read data from the SMC
    
:param: key The SMC key
*/
static kern_return_t read_smc(char *key, smc_return_t *result_smc)
{
    kern_return_t result;
    SMCParamStruct inputStruct;
    SMCParamStruct outputStruct;

    memset(&inputStruct,  0, sizeof(SMCParamStruct));
    memset(&outputStruct, 0, sizeof(SMCParamStruct));
    memset(result_smc,    0, sizeof(smc_return_t));

    // First call to AppleSMC - get key info
    inputStruct.key = to_uint32_t(key);
    inputStruct.data8 = kSMCGetKeyInfo;

    result = call_smc(&inputStruct, &outputStruct);
    result_smc->kSMC = outputStruct.result;       
    
    if (result != kIOReturnSuccess || outputStruct.result != kSMCSuccess) {
        return result;
    }

    // Store data for return
    result_smc->dataSize = outputStruct.keyInfo.dataSize;
    to_string(outputStruct.keyInfo.dataType, result_smc->dataType);
    
    // Second call to AppleSMC - now we can get the data
    inputStruct.keyInfo.dataSize = outputStruct.keyInfo.dataSize;
    inputStruct.data8 = kSMCReadKey;

    result = call_smc(&inputStruct, &outputStruct);
    result_smc->kSMC = outputStruct.result;       
    
    if (result != kIOReturnSuccess || outputStruct.result != kSMCSuccess) {
        return result;
    }

    memcpy(result_smc->data, outputStruct.bytes, sizeof(outputStruct.bytes));

    return result;
}


/**
Write data to the SMC.

:returns: IOReturn IOKit return code
*/
static kern_return_t write_smc(char *key, smc_return_t *result_smc)
{
    kern_return_t result;
    SMCParamStruct inputStruct;
    SMCParamStruct outputStruct;

    memset(&inputStruct,  0, sizeof(SMCParamStruct));
    memset(&outputStruct, 0, sizeof(SMCParamStruct));

    // First call to AppleSMC - get key info
    inputStruct.key = to_uint32_t(key);
    inputStruct.data8 = kSMCGetKeyInfo;

    result = call_smc(&inputStruct, &outputStruct);
    result_smc->kSMC = outputStruct.result;       
    
    if (result != kIOReturnSuccess || outputStruct.result != kSMCSuccess) {
        return result;
    }
    
    // Check data is correct
    // TODO: Add dataType check
    if (result_smc->dataSize != outputStruct.keyInfo.dataSize) {
        return kIOReturnBadArgument;
    }

    // Second call to AppleSMC - now we can write the data
    inputStruct.data8 = kSMCWriteKey;
    inputStruct.keyInfo.dataSize = outputStruct.keyInfo.dataSize;
    
    // Set data to write
    memcpy(outputStruct.bytes, result_smc->data, sizeof(result_smc->data));
    
    result = call_smc(&inputStruct, &outputStruct);
    result_smc->kSMC = outputStruct.result;       
    
    return result;
}

//------------------------------------------------------------------------------
// MARK: "PUBLIC" FUNCTIONS
//------------------------------------------------------------------------------


/**
Open a connection to the SMC
    
:returns: kIOReturnSuccess on successful connection to the SMC.
*/
kern_return_t open_smc(void)
{
    kern_return_t result;
    io_service_t service;

    service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                          IOServiceMatching(IOSERVICE_SMC));
   
    if (service == 0) {
        // NOTE: IOServiceMatching documents 0 on failure
        printf("ERROR: %s NOT FOUND\n", IOSERVICE_SMC);
        return kIOReturnError;
    }

    result = IOServiceOpen(service, mach_task_self(), 0, &conn);
    IOObjectRelease(service);

    return result;
}


/**
Close connection to the SMC
    
:returns: kIOReturnSuccess on successful close of connection to the SMC.
*/
kern_return_t close_smc(void)
{
    return IOServiceClose(conn);
}


/**
Check if an SMC key is valid. Useful for determining if a certain machine has
particular sensor or fan for example.
    
:param: key The SMC key to check. 4 byte multi-character constant. Must be 4
            characters in length.
:returns: True if the key is found, false otherwise
*/
bool is_key_valid(char *key)
{
    bool ans = false;
    kern_return_t result;
    smc_return_t  result_smc;
  
    if (strlen(key) != SMC_KEY_SIZE) {
        printf("ERROR: Invalid key size - must be 4 chars\n");
        return ans;
    }
 
    // Try a read and see if it succeeds
    result = read_smc(key, &result_smc);
    
    if (result == kIOReturnSuccess && result_smc.kSMC == kSMCSuccess) {
        ans = true;
    }

    return ans;
}


/**
Get the current temperature from a sensor
    
:param: key The temperature sensor to read from
:param: unit The unit for the temperature value.
:returns: Temperature of sensor. If the sensor is not found, or an error
          occurs, return will be zero
*/
double get_tmp(char *key, tmp_unit_t unit)
{
    kern_return_t result;
    smc_return_t  result_smc;

    result = read_smc(key, &result_smc);
    
    if (!(result == kIOReturnSuccess &&
          result_smc.dataSize == 2   &&
          strcmp(result_smc.dataType, DATA_TYPE_SP78) == 0)) {
        // Error
        return 0.0;
    }
    
    // TODO: Create from_sp78() convert function
    double tmp = result_smc.data[0];      
  
    switch (unit) {
        case CELSIUS:
            break;
        case FAHRENHEIT:
            tmp = to_fahrenheit(tmp);
            break;
        case KELVIN:
            tmp = to_kelvin(tmp);
            break;
    }

    return tmp;
}


//------------------------------------------------------------------------------
// MARK: FAN FUNCTIONS
//------------------------------------------------------------------------------


/**
Get the number of fans on this machine.

:returns: The number of fans. If an error occurs, return will be -1.
*/
int get_num_fans(void)
{
    kern_return_t result;
    smc_return_t  result_smc;

    result = read_smc("FNum", &result_smc);
    
    if (!(result == kIOReturnSuccess &&
          result_smc.dataSize == 1   &&
          strcmp(result_smc.dataType, DATA_TYPE_UINT8) == 0)) {
        // Error
        return -1;
    }

    return result_smc.data[0];
}


/**
Get the current speed (RPM - revolutions per minute) of a fan.
    
:param: fan_num The number of the fan to check
:returns: The fan RPM. If the fan is not found, or an error occurs, return
          will be zero
*/
unsigned int get_fan_rpm(unsigned int fan_num)
{
    kern_return_t result;
    smc_return_t  result_smc;

    // FIXME: Use fan_num for key
    result = read_smc("F0Ac", &result_smc);

    if (!(result == kIOReturnSuccess &&
          result_smc.dataSize == 2   &&
          strcmp(result_smc.dataType, DATA_TYPE_FPE2) == 0)) {
        // Error
        return 0;
    }

    return from_fpe2(result_smc.data);
}


/**
Set the speed (RPM - revolutions per minute) of a fan. This method requires root
privlages.

WARNING: You are playing with hardware here, BE CAREFUL.

:param: fan_num The number of the fan to set
:param: rpm The speed you would like to set the fan to.
:param: auth Should the function do authentication?
:return: True if successful, false otherwise
*/
bool set_fan_min_rpm(unsigned int fan_num, unsigned int rpm, bool auth)
{
    // TODO: Add rpm val safety check
    bool ans = false;
    kern_return_t result;
    smc_return_t  result_smc;
    
    memset(&result_smc, 0, sizeof(smc_return_t));

    // TODO: Don't use magic number
    // TODO: Set result_smc.dataType
    result_smc.dataSize = 2;
    to_fpe2(rpm, result_smc.data);

    // FIXME: Use fan_num for key
    result = write_smc("F0Mn", &result_smc);
    
    if (result == kIOReturnSuccess && result_smc.kSMC == kSMCSuccess) {
        ans = true;
    }    
 
    return ans;
}
