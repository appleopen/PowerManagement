/*
 * Copyright (c) 2001 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 2001 Apple Computer, Inc.  All rights reserved. 
 *
 * HISTORY
 *
 * 18-Dec-01 ebold created
 *
 */

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCValidation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>

#include <string.h>
#include <ctype.h>

/* 
 * This is the command line interface to Energy Saver Preferences in
 * /var/db/SystemConfiguration/com.apple.PowerManagement.xml
 *
 * Courtesy of Soren Spies:
 * usage as of Mon Oct 29, 2001  06:19:43 PM
 Usage: pmset [-b | -c | -a] <action> <minutes> [[<opts>] <action> <minutes> ...]
       -c adjust settings used while connected to a charger
       -b adjust settings used when running off a battery
       -a (default) adjust settings for both
       <action> is one of: dim, sleep, spindown, slower, womp* (* flag = 1/0)
       eg. pmset womp 1 -c dim 5 sleep 15 -b dim 3 spindown 5 sleep 8
 *
 *
 *
 * usage as of Tues Oct 30, 2001, 09:23:34 PM
 Usage:  pmset [-b | -c | -a] <action> <minutes> [[<opts>] <action> <minutes>...]
                -c adjust settings used while connected to a charger
                -b adjust settings used when running off a battery 
                -a (default) adjust settings for both
                <action> is one of: dim, sleep, spindown, slower
                eg. pmset -c dim 5 sleep 15 -b dim 3 spindown 5 sleep 8
         pmset <setting> <time> [<setting> <time>]
                <setting> is one of: wakeat, sleepat
                <time> is in 24 hour format
         pmset womp [ on | off ]
 *
 */

// CLArgs
#define	ARG_DIM             "dim"
#define ARG_SLEEP           "sleep"
#define ARG_SPINDOWN        "spindown"
#define ARG_REDUCE          "slower"
#define ARG_WOMP            "womp"
#define ARG_BOOT            "boot"
#define ARG_POWERBUTTON     "powerbutton"

#define ARG_REDUCE2         "reduce"
#define ARG_DPS             "dps"
#define ARG_RING            "ring"
#define ARG_AUTORESTART     "autorestart"

// get options
#define ARG_CAP             "cap"
#define ARG_DISK            "disk"
#define ARG_LIVE            "live"

// return values for parseArgs
#define kParseBadArgs                   -1
#define kParseMadeNoChanges             -2

// return values for idleSettingsNotConsistent
#define kInconsistentDisplaySetting     1
#define kInconsistentDiskSetting        2
#define kConsistentSleepSettings        0

#define kNUM_PM_FEATURES	8
/* list of all features */
char	*all_features[kNUM_PM_FEATURES] =
{ 
    kIOPMDisplaySleepKey, 
    kIOPMDiskSleepKey, 
    kIOPMSystemSleepKey, 
    kIOPMReduceSpeedKey, 
    kIOPMDynamicPowerStepKey, 
    kIOPMWakeOnLANKey, 
    kIOPMWakeOnRingKey, 
    kIOPMRestartOnPowerLossKey
};

enum ArgumentType {
    ApplyToBattery = 1,
    ApplyToCharger = 2
};

static void usage(void)
{
    printf("Usage:  pmset [-b | -c | -a] <action> <minutes> [[<opts>] <action> <minutes>...]\n");
    printf("        pmset -g [disk | cap | live]\n");
    printf("                -c adjust settings used while connected to a charger\n");
    printf("                -b adjust settings used when running off a battery\n");
    printf("                -a (default) adjust settings for both\n");
    printf("                <action> is one of: dim, sleep, spindown (with an argument of minutes)\n");
    printf("                    or: reduce, dps, womp, ring, autorestart (with a 1 or 0 argument)\n");
    printf("                eg. pmset -c dim 5 sleep 15 spindown 10 autorestart 1 womp 1\n");
}

static IOReturn setRootDomainProperty(CFStringRef key, CFTypeRef val) {
    mach_port_t                 masterPort;
    io_iterator_t               it;
    io_registry_entry_t         root_domain;
    IOReturn                    ret;
    
    IOMasterPort(bootstrap_port, &masterPort);
    if(!masterPort) return kIOReturnError;
    IOServiceGetMatchingServices(masterPort, IOServiceNameMatching("IOPMrootDomain"), &it);
    if(!it) return kIOReturnError;
    root_domain = (io_registry_entry_t)IOIteratorNext(it);
    if(!root_domain) return kIOReturnError;
    
    ret = IORegistryEntrySetCFProperty(root_domain, key, val);
    
    IOObjectRelease(root_domain);
    IOObjectRelease(it);
    IOObjectRelease(masterPort);
    return ret;
}

static io_registry_entry_t getCudaPMURef(void)
{
    io_iterator_t	            tmp = NULL;
    io_registry_entry_t	        cudaPMU = NULL;
    mach_port_t                 masterPort;
    
    IOMasterPort(bootstrap_port,&masterPort);
    
    // Search for PMU
    IOServiceGetMatchingServices(masterPort, IOServiceNameMatching("ApplePMU"), &tmp);
    if(tmp) {
        cudaPMU = IOIteratorNext(tmp);
        //if(cudaPMU) magicCookie = kAppleMagicPMUCookie;
        IOObjectRelease(tmp);
    }

    // No? Search for Cuda
    if(!cudaPMU) {
        IOServiceGetMatchingServices(masterPort, IOServiceNameMatching("AppleCuda"), &tmp);
        if(tmp) {
            cudaPMU = IOIteratorNext(tmp);
            //if(cudaPMU) magicCookie = kAppleMagicCudaCookie;
            IOObjectRelease(tmp);
        }
    }
    return cudaPMU;
}


// Todo: PMFeatureIsSupported is duplicated code from 
// IOKitUser/pwr_mgt.subproj/IOPMEnergyPrefs.c:IOPMFunctionIsAvailable
// We should make the IOKitUser function
// available as a private header and eliminate this one.
static int PMFeatureIsSupported(CFStringRef f,CFStringRef power_source)
{
    CFDictionaryRef		        supportedFeatures = NULL;
    CFPropertyListRef           izzo;
    io_registry_entry_t		    registry_entry;
    io_iterator_t		        tmp;
    mach_port_t                   masterPort;
    IOReturn                    ret = 0;

    IOMasterPort(bootstrap_port, &masterPort);    
    
    IOServiceGetMatchingServices(masterPort, IOServiceNameMatching("IOPMrootDomain"), &tmp);
    registry_entry = IOIteratorNext(tmp);
    IOObjectRelease(tmp);
    
    supportedFeatures = IORegistryEntryCreateCFProperty(registry_entry, CFSTR("Supported Features"),
                            kCFAllocatorDefault, NULL);
    IOObjectRelease(registry_entry);
    
    if(CFEqual(f, CFSTR(kIOPMDisplaySleepKey))
        || CFEqual(f, CFSTR(kIOPMSystemSleepKey))
        || CFEqual(f, CFSTR(kIOPMDiskSleepKey)))
    {
        ret = 1;
        goto IOPMFunctionIsAvailable_exitpoint;
    }
    
    // reduce processor speed
    if(CFEqual(f, CFSTR(kIOPMReduceSpeedKey)))
    {
        if(!supportedFeatures) return kIOReturnError;
        if(CFDictionaryGetValue(supportedFeatures, f))
            ret = 1;
        else ret = 0;
        goto IOPMFunctionIsAvailable_exitpoint;
    }

    // dynamic powerstep
    if(CFEqual(f, CFSTR(kIOPMDynamicPowerStepKey)))
    {
        if(!supportedFeatures) return kIOReturnError;
        if(CFDictionaryGetValue(supportedFeatures, f))
            ret = 1;
        else ret = 0;
        goto IOPMFunctionIsAvailable_exitpoint;
    }

    // wake on magic packet
    if(CFEqual(f, CFSTR(kIOPMWakeOnLANKey)))
    {
        // Check for WakeOnLAN property in supportedFeatures
        // Radar 2946434 WakeOnLAN is only supported when running on AC power. It's automatically disabled
        // on battery power, and thus shouldn't be offered as a checkbox option.
        if(CFDictionaryGetValue(supportedFeatures, CFSTR("WakeOnMagicPacket"))
                && (!power_source || !CFEqual(CFSTR(kIOPMBatteryPowerKey), power_source)))
        {
            ret = 1;
        } else {
            ret = 0;
        }
        goto IOPMFunctionIsAvailable_exitpoint;       
    }

    if(CFEqual(f, CFSTR(kIOPMWakeOnRingKey)))
    {
        // Check for WakeOnRing property under PMU
        registry_entry = getCudaPMURef();
        if((izzo = IORegistryEntryCreateCFProperty(registry_entry, CFSTR("WakeOnRing"),
                            kCFAllocatorDefault, NULL)))
        {
            CFRelease(izzo);
            IOObjectRelease(registry_entry);
            ret = 1;
        } else {
            IOObjectRelease(registry_entry);    
            ret = 0;
        }
        goto IOPMFunctionIsAvailable_exitpoint;        
    }

    // restart on power loss
    if(CFEqual(f, CFSTR(kIOPMRestartOnPowerLossKey)))
    {
        registry_entry = getCudaPMURef();
        // Check for fileserver property under PMU
        if((izzo = IORegistryEntryCreateCFProperty(registry_entry, CFSTR("FileServer"),
                            kCFAllocatorDefault, NULL)))
        { 
            CFRelease(izzo);
            IOObjectRelease(registry_entry);
            ret = 1;
        } else {
            IOObjectRelease(registry_entry);       
            ret = 0;
        }
        goto IOPMFunctionIsAvailable_exitpoint;
    }
        
 IOPMFunctionIsAvailable_exitpoint:
    if(supportedFeatures) CFRelease(supportedFeatures);
    IOObjectRelease(masterPort);
    return ret;
}

static void print_setting_value(CFTypeRef a)
{
    int n;
    
    if(isA_CFNumber(a))
    {
        CFNumberGetValue(a, kCFNumberIntType, (void *)&n);
        printf("%d", n);    
    } else if(isA_CFBoolean(a))
    {
	printf("%d", CFBooleanGetValue(a));    
    } else printf("oops - print_setting_value came unprepared\n");
}

static void show_pm_settings_dict(CFDictionaryRef d)
{
    int 				count;
    int					i;
    char				*ps;
    CFStringRef				*keys;
    CFTypeRef				*vals;

    count = CFDictionaryGetCount(d);
    keys = (CFStringRef *)malloc(count * sizeof(void *));
    vals = (CFTypeRef *)malloc(count * sizeof(void *));
    if(!keys || !vals) return;
    CFDictionaryGetKeysAndValues(d, (const void **)keys, (const void **)vals);

    for(i=0; i<count; i++)
    {
        ps = (char *)CFStringGetCStringPtr(keys[i], 0);
        if(!ps) continue; // with for loop

	if (strcmp(ps, kIOPMDisplaySleepKey) == 0)
		    printf(" dim\t\t");  
	else if (strcmp(ps, kIOPMDiskSleepKey) == 0)
		    printf(" spindown\t");  
	else if (strcmp(ps, kIOPMSystemSleepKey) == 0)
		    printf(" sleep\t\t");  
	else if (strcmp(ps, kIOPMWakeOnLANKey) == 0)
		    printf(" womp\t\t");  
	else if (strcmp(ps, kIOPMWakeOnRingKey) == 0)
		    printf(" ring\t\t");  
	else if (strcmp(ps, kIOPMRestartOnPowerLossKey) == 0)
		    printf(" autorestart\t");  
	else if (strcmp(ps, kIOPMReduceSpeedKey) == 0)
		    printf(" reduce\t\t");  
	else if (strcmp(ps, kIOPMDynamicPowerStepKey) == 0)
		    printf(" dps\t\t");  
	else
		  printf("Error.  Unknown string.\n");
  
        print_setting_value(vals[i]);  
        printf("\n");
    }
    free(keys);
    free(vals);
}

static void show_supported_pm_features(char *power_source) 
{
    int i;
    CFStringRef feature;
    CFStringRef source;

    printf("Capabilities:\n");
    // iterate the list of all features
    for(i=0; i<kNUM_PM_FEATURES; i++)
    {
        feature = CFStringCreateWithCStringNoCopy(NULL, all_features[i], NULL, kCFAllocatorNull);
        if(!isA_CFString(feature)) return;
        source = CFStringCreateWithCStringNoCopy(NULL, power_source, NULL, kCFAllocatorNull);
        if(!isA_CFString(source)) return;
        if( PMFeatureIsSupported(feature, source) )
        {

	  if (strcmp(all_features[i], kIOPMSystemSleepKey) == 0)
	    printf(" sleep\n");  
	  else if (strcmp(all_features[i], kIOPMRestartOnPowerLossKey) == 0)
	    printf(" autorestart\n");  
	  else if (strcmp(all_features[i], kIOPMDiskSleepKey) == 0)
	    printf(" spindown\n");  
	  else if (strcmp(all_features[i], kIOPMWakeOnLANKey) == 0)
	    printf(" womp\n");  
	  else if (strcmp(all_features[i], kIOPMWakeOnRingKey) == 0)
	    printf(" ring\n");  
	  else if (strcmp(all_features[i], kIOPMDisplaySleepKey) == 0)
	    printf(" dim\n");  
	  else if (strcmp(all_features[i], kIOPMReduceSpeedKey) == 0)
	    printf(" reduce\n");  
	  else if (strcmp(all_features[i], kIOPMDynamicPowerStepKey) == 0)
	    printf(" dps\n");  
	  else
	    printf("Error.  Unknown capability string.\n");

        }
        CFRelease(feature);
        CFRelease(source);
    }
}

static void show_disk_pm_settings(void)
{
    CFDictionaryRef		es = NULL;
    int				num_profiles, num_newlines;
    int				i;
    char 			*ps;
    CFStringRef			*keys;
    CFDictionaryRef		*values;

    // read settings file from /var/db/SystemConfiguration/com.apple.PowerManagement.xml
    es = IOPMCopyPMPreferences();
    if(!isA_CFDictionary(es)) return;
    
    num_profiles = CFDictionaryGetCount(es);
    keys = (CFStringRef *)malloc(num_profiles * sizeof(void *));
    values = (CFDictionaryRef *)malloc(num_profiles * sizeof(void *));
    if(!keys || !values) return;
    CFDictionaryGetKeysAndValues(es, (const void **)keys, (const void **)values);
    
    num_newlines = num_profiles-1;
    for(i=0; i<num_profiles; i++)
    {
        ps = (char *)CFStringGetCStringPtr(keys[i], 0);
        if(!ps) continue; // with for loop
        printf("%s:\n", ps);
        show_pm_settings_dict(values[i]);
        if(--num_newlines > 0)
            printf("\n");
    }

    free(keys);
    free(values);
    CFRelease(es);
}

static void show_live_pm_settings(void)
{
    SCDynamicStoreRef		ds;
    CFDictionaryRef		live;

    ds = SCDynamicStoreCreate(NULL, CFSTR("pmset"), NULL, NULL);

    // read current settings from SCDynamicStore key
    live = SCDynamicStoreCopyValue(ds, CFSTR(kIOPMDynamicStoreSettingsKey));
    if(!isA_CFDictionary(live)) return;
    printf("Currently in use:\n");
    show_pm_settings_dict(live);

    CFRelease(live);
    CFRelease(ds);
}

static int checkAndSetIntValue(char *valstr, CFStringRef settingKey, int apply,
                int isOnOffSetting, CFMutableDictionaryRef ac, CFMutableDictionaryRef batt)
{
    CFNumberRef     cfnum;
    char            *endptr = NULL;
    long            val;

    if(!valstr) return -1;

    val = strtol(valstr, &endptr, 10);

    if(0 != *endptr)
    {
        // the string contained some non-numerical characters - bail
        return -1;
    }
    
    // for on/off settings, turn any non-zero number into a 1
    if(isOnOffSetting) val = (val?1:0);

    cfnum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &val);
    if(!cfnum) return -1;
    if(apply & ApplyToBattery)
        CFDictionarySetValue(batt, settingKey, cfnum);
    if(apply & ApplyToCharger)
        CFDictionarySetValue(ac, settingKey, cfnum);
    CFRelease(cfnum);
    return 0;
}

static int parseArgs(int argc, char* argv[], CFMutableDictionaryRef settings)
{
    int i = 1;
    int j;
    int apply = 0;
    IOReturn kr;
    CFMutableDictionaryRef	battery = NULL;
    CFMutableDictionaryRef	ac = NULL;

    if(argc == 1)
        return kParseBadArgs;
    
    if(!settings) {
        return kParseBadArgs;
    }
    
    // Either battery or AC settings may not exist if the system doesn't support it.
    battery = (CFMutableDictionaryRef)CFDictionaryGetValue(settings, CFSTR(kIOPMBatteryPowerKey));
    ac = (CFMutableDictionaryRef)CFDictionaryGetValue(settings, CFSTR(kIOPMACPowerKey));
    
    // Unless specified, apply changes to both battery and AC
    if(battery) apply |= ApplyToBattery;
    if(ac) apply |= ApplyToCharger;
    
    while(i < argc)
    {
        // I only speak lower case.
        for(j=0; j<strlen(argv[i]); j++)
        {
            tolower(argv[i][j]);
        }
    
        if(argv[i][0] == '-')
        {
        // Process -a/-b/-c/-g arguments
            apply = 0;
            switch (argv[i][1])
            {
                case 'a':
                    if(battery) apply |= ApplyToBattery;
                    if(ac) apply |= ApplyToCharger;
                    break;
                case 'b':
                    if(battery) apply = ApplyToBattery;
                    break;
                case 'c':
                    if(ac) apply = ApplyToCharger;
                    break;
                case 'g':
                    // One of the "gets"
                    i++;
                    
                    // is the next word NULL? then it's a "-g" arg
                    if( (NULL == argv[i])
                        || !strcmp(argv[i], ARG_LIVE))
                    {
                        // show settings on disk
                        show_live_pm_settings();
                    } else if(!strcmp(argv[i], ARG_DISK))        
                    {
                        // show live settings
                        show_disk_pm_settings();
                    } else if(!strcmp(argv[i], ARG_CAP))
                    {
                        // show capabilities (when the machine is running on AC power)
                        show_supported_pm_features(kIOPMACPowerKey);
                    }
                    
                    // return immediately - don't handle any more setting arguments
                    return kParseMadeNoChanges;
                    
                    break;
                default:
                    // bad!
                    return kParseBadArgs;
                    break;
            }
            
            i++;
        } else
        {
        // Process the settings
            if(0 == strcmp(argv[i], ARG_BOOT))
            {
                // Tell kernel power management that bootup is complete
                kr = setRootDomainProperty(CFSTR("System Boot Complete"), kCFBooleanTrue);
                if(kr != kIOReturnSuccess) {
                    printf("pmset: Error 0x%x setting boot property\n", kr);
                }

                i++;
            } else if(0 == strcmp(argv[i], ARG_POWERBUTTON))
            {
                CFBooleanRef    b;
                // Tell IOPMrootDomain to ignore/pay attention to power button sleep requests
                if(argv[i+1])
                {
                    char            *endptr = NULL;
                    long            val;

                    val = strtol(argv[i+1], &endptr, 10);

                    if((0 != *endptr) || !val)
                        b = kCFBooleanFalse;
                    else b = kCFBooleanTrue;
                }    

                kr= setRootDomainProperty(CFSTR("DisablePowerButtonSleep"), b);
                if(kr != kIOReturnSuccess) {
                    printf("pmset: Error 0x%x setting DisablePowerButtonSleep property\n", kr);
                }
                        
                i+=2;
            } else if(0 == strcmp(argv[i], ARG_DIM))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMDisplaySleepKey), apply, 0, ac, battery))
                    return kParseBadArgs;
                i+=2;
            } else if(0 == strcmp(argv[i], ARG_SPINDOWN))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMDiskSleepKey), apply, 0, ac, battery))
                    return kParseBadArgs;                    
                i+=2;
            } else if(0 == strcmp(argv[i], ARG_SLEEP))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMSystemSleepKey), apply, 0, ac, battery))
                    return kParseBadArgs;
                i+=2;
            } else if( (0 == strcmp(argv[i], ARG_REDUCE))
                        || (0 == strcmp(argv[i], ARG_REDUCE2)) )
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMReduceSpeedKey), apply, 1, ac, battery))
                    return kParseBadArgs;
                i+=2;
            } else if(0 == strcmp(argv[i], ARG_WOMP))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMWakeOnLANKey), apply, 1, ac, battery))
                    return kParseBadArgs;
                i+=2;
            } else if(0 == strcmp(argv[i], ARG_DPS))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMDynamicPowerStepKey), apply, 1, ac, battery))
                    return kParseBadArgs;
                i+=2;
            } else if(0 == strcmp(argv[i], ARG_RING))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMWakeOnRingKey), apply, 1, ac, battery))
                    return kParseBadArgs;
                i+=2;
            } else if(0 == strcmp(argv[i], ARG_AUTORESTART))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMRestartOnPowerLossKey), apply, 1, ac, battery))
                    return kParseBadArgs;
                i+=2;
            } else return kParseBadArgs;
        } // if
    } // while
    
    if(battery) CFDictionarySetValue(settings, CFSTR(kIOPMBatteryPowerKey), battery);
    if(ac) CFDictionarySetValue(settings, CFSTR(kIOPMACPowerKey), ac);
    
    return 0;
}

// int arePowerSourceSettingsInconsistent(CFDictionaryRef)
// Function - determine if the settings will produce the "intended" idle sleep consequences
// Parameter - The CFDictionary contains energy saver settings of 
//             CFStringRef keys and CFNumber/CFBoolean values
// Return - non-zero bitfield, each bit indicating a setting inconsistency
//          0 if settings will produce expected result 
//         -1 in case of other error
static int arePowerSourceSettingsInconsistent(CFDictionaryRef set)
{
    int                 sleep_time, disk_time, dim_time;
    CFNumberRef         num;
    int                 ret = 0;

    num = isA_CFNumber(CFDictionaryGetValue(set, CFSTR(kIOPMSystemSleepKey)));
    if(!num || !CFNumberGetValue(num, kCFNumberIntType, &sleep_time)) return -1;
    
    num = isA_CFNumber(CFDictionaryGetValue(set, CFSTR(kIOPMDisplaySleepKey)));
    if(!num || !CFNumberGetValue(num, kCFNumberIntType, &dim_time)) return -1;

    num = isA_CFNumber(CFDictionaryGetValue(set, CFSTR(kIOPMDiskSleepKey)));
    if(!num || !CFNumberGetValue(num, kCFNumberIntType, &disk_time)) return -1;

    // For system sleep to occur around the time you set it to, the disk and display
    // sleep timers must conform to these rules:
    // 1. display sleep <= system sleep
    // 2. If system sleep != Never, then disk sleep can not be == Never
    //    2a. It is, however, OK for disk sleep > system sleep. A funky hack in
    //        the kernel IOPMrootDomain allows this.
    // and note: a time of zero means "never power down"
    
    if(sleep_time != 0)
    {
        if(dim_time > sleep_time || dim_time == 0) ret |= kInconsistentDisplaySetting;
    
        if(disk_time == 0) ret |= kInconsistentDiskSetting;
    }
    
    return ret;
}

// void checkSettingConsistency(CFDictionaryRef)
// Checks ES settings profile by profile
// Prints a user warning if any idle timer settings violate the kernel's assumptions
// about idle, disk, and display sleep timers.
// Parameter - a CFDictionary of energy settings "profiles", usually one per power supply
static void checkSettingConsistency(CFDictionaryRef profiles)
{
    int                 num_profiles;
    int                 i;
    int                 ret;
    char                buf[100];
    CFStringRef			*keys;
    CFDictionaryRef		*values;
    
    num_profiles = CFDictionaryGetCount(profiles);
    keys = (CFStringRef *)malloc(num_profiles * sizeof(void *));
    values = (CFDictionaryRef *)malloc(num_profiles * sizeof(void *));
    if(!keys || !values) return;
    CFDictionaryGetKeysAndValues(profiles, (const void **)keys, (const void **)values);
    
    for(i=0; i<num_profiles; i++)
    {
        // Check settings profile by profile
        if(ret = arePowerSourceSettingsInconsistent(values[i]))
        {
            // get profile name
            if(!CFStringGetCString(keys[i], buf, 100, NULL)) break;
            
            fprintf(stderr, "Warning: Idle sleep timings for \"%s\" may not behave as expected.\n", buf);
            if(ret & kInconsistentDisplaySetting)
            {
                fprintf(stderr, "- Display sleep should have a lower timeout than system sleep.\n");
            }
            if(ret & kInconsistentDiskSetting)
            {
                fprintf(stderr, "- Disk sleep should be non-zero whenever system sleep is non-zero.\n");
            }
            fflush(stderr);        
        }
    }

    free(keys);
    free(values);
}

int main(int argc, char *argv[]) {
    IOReturn			ret;
    CFMutableDictionaryRef	es;

    if(!(es=IOPMCopyPMPreferences()))
    {
        printf("pmset: error getting profiles\n");
    }
    
    ret = parseArgs(argc, argv, es);

    if(ret == kParseBadArgs)
    {
        //printf("pmset: error in parseArgs!\n");
        usage();
        return 0;
    }
    
    if(ret == kParseMadeNoChanges)
    {
        return 0;
    }
    
    // Send pmset changes out to disk
    if(kIOReturnSuccess != (ret = IOPMSetPMPreferences(es)))
    {
        if(ret == kIOReturnNotPrivileged)
            printf("\'%s\' must be run as root...\n", argv[0]);
        if(ret == kIOReturnError)
            printf("Error writing preferences to disk\n");
        exit(1);
    }
    
    // Print a warning to stderr if idle sleep settings won't produce expected result
    checkSettingConsistency(es);    

    CFRelease(es);
    return 0;
}
