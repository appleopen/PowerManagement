/*
 * Copyright (c) 2013-2014 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <CoreFoundation/CoreFoundation.h>
#include <asl.h>
#include <mach/mach.h>
#include <notify.h>

#include "PMAssertions.h"
#include "PrivateLib.h"

#include <IOReport.h>

#define DISPLAY_ON_ASSERTION_LOG_DELAY         (60LL)
#define DISPLAY_OFF_ASSERTION_LOG_DELAY        (10LL)

#define PERIODIC_LOG_INTERVAL                  (15*60)  // 15min

#define AA_MAX_ENTRIES             512

extern os_log_t    assertions_log;
#undef   LOG_STREAM
#define  LOG_STREAM   assertions_log

extern assertionType_t              gAssertionTypes[];
extern CFMutableDictionaryRef       gProcessDict;
extern uint32_t                     gDebugFlags;
extern uint32_t                     gActivityAggCnt;


typedef struct {
    CFDateRef               startTime;
    CFMutableArrayRef       types;         
} assertionAggregate_t;

typedef struct {
    uint32_t                idx;
    CFMutableArrayRef       log;
    uint32_t                unreadCnt;  // Number of entries logged since last read by
                                        // entitled reader. There should be only one entitled
                                        // reader in the system.
} assertionActivity_t;

assertionActivity_t     activity;
assertionAggregate_t    aggregate;
static  uint32_t        gActivityLogCnt = 0;  // Has to be explicity enabled on OSX


#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
__private_extern__ bool isDisplayAsleep( );

static void logAssertionActivity(assertLogAction  action,
                                 assertion_t     *assertion)
{

    bool            logBT = false;
    CFDateRef       time = NULL;
    CFStringRef     actionStr = NULL;
    CFNumberRef     pid_cf = NULL, retain_cf = NULL, uniqueAID = NULL;
    CFTypeRef       type = NULL, name = NULL, btSymbols = NULL;
    CFTypeRef       onBehalfPid = NULL, onBehalfPidStr = NULL;
    CFTypeRef       onBehalfBundleID = NULL;

    CFMutableDictionaryRef  entry = NULL;
    CFDictionaryRef         props = assertion->props;

    switch(action) {

    case kACreateLog:
        logBT = true;
        actionStr = CFSTR(kPMASLAssertionActionCreate);
        break;

    case kACreateRetain:
        logBT = true;
        actionStr = CFSTR(kPMASLAssertionActionRetain);
        break;

    case kATurnOnLog:
        logBT = true;
        actionStr = CFSTR(kPMASLAssertionActionTurnOn);
        break;

    case kAReleaseLog:
        actionStr = CFSTR(kPMASLAssertionActionRelease);
        break;

    case kAClientDeathLog:
        actionStr = CFSTR(kPMASLAssertionActionClientDeath);
        break;

    case kATimeoutLog:
        actionStr = CFSTR(kPMASLAssertionActionTimeOut);
        break;

    case kATurnOffLog:
        actionStr = CFSTR(kPMASLAssertionActionTurnOff);
        break;

    case kANameChangeLog:
        if (!(gDebugFlags & kIOPMDebugLogAssertionNameChange)) {
            return;
        }
        actionStr = CFSTR(kPMASLAssertionActionNameChange);
        break;

    default:
        return;
    }

    if (!activity.log) {
        activity.log = CFArrayCreateMutable(NULL, AA_MAX_ENTRIES, &kCFTypeArrayCallBacks);

        if (!activity.log) return;

        activity.unreadCnt = UINT_MAX;
        // Send a high water mark notification to force a read by powerlog after powerd's crash
        notify_post(kIOPMAssertionsLogBufferHighWM);
    }

    entry = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, 
                                      &kCFTypeDictionaryValueCallBacks);
    if (!entry) return;

    // Current time of this activity
    time = CFDateCreate(0, CFAbsoluteTimeGetCurrent());
    if (time) {
        CFDictionarySetValue(entry, kIOPMAssertionActivityTime, time);
        CFRelease(time);
    }
    else {
        // Not much to log, if we can't even get the time of activity
        CFRelease(entry);
        return;
    }

    // Assertion type
    if ((type = CFDictionaryGetValue(props, kIOPMAssertionTypeKey)) != NULL)
        CFDictionarySetValue(entry, kIOPMAssertionTypeKey, type);

    // Assertion name
    if ((name = CFDictionaryGetValue(props, kIOPMAssertionNameKey)) != NULL)
        CFDictionarySetValue(entry, kIOPMAssertionNameKey, name);

    // Type of assertion action
    CFDictionarySetValue(entry, kIOPMAssertionActivityAction, actionStr);

    // PID owning this assertion
    if ((pid_cf = CFNumberCreate(NULL, kCFNumberIntType, &assertion->pinfo->pid)) != NULL) {
        CFDictionarySetValue(entry, kIOPMAssertionPIDKey, pid_cf);
        CFRelease(pid_cf);
    }
      
    // Retain count
    if ((retain_cf = CFNumberCreate(NULL, kCFNumberIntType, &assertion->retainCnt)) != NULL) {
        CFDictionarySetValue(entry, kIOPMAssertionRetainCountKey, retain_cf);
        CFRelease(retain_cf);
    }

    // Assertion ID
    if ((uniqueAID = CFDictionaryGetValue(props, kIOPMAssertionGlobalUniqueIDKey)) != NULL) 
        CFDictionarySetValue(entry, kIOPMAssertionGlobalUniqueIDKey, uniqueAID);

    // Assertion on behalf of PID
    if ((onBehalfPid = CFDictionaryGetValue(props, kIOPMAssertionOnBehalfOfPID)) != NULL) 
        CFDictionarySetValue(entry, kIOPMAssertionOnBehalfOfPID, onBehalfPid);

    if ((onBehalfPidStr = CFDictionaryGetValue(props, kIOPMAssertionOnBehalfOfPIDReason)) != NULL) 
        CFDictionarySetValue(entry, kIOPMAssertionOnBehalfOfPIDReason, onBehalfPidStr);

    // Assertion on behalf of Bundle ID
    if ((onBehalfBundleID = CFDictionaryGetValue(props, kIOPMAssertionOnBehalfOfBundleID)) != NULL)
        CFDictionarySetValue(entry, kIOPMAssertionOnBehalfOfBundleID, onBehalfBundleID);

    if (logBT) {
        // Backtrace of assertion creation
        if ((btSymbols = CFDictionaryGetValue(props, kIOPMAssertionCreatorBacktrace)) != NULL)
            CFDictionarySetValue(entry, kIOPMAssertionCreatorBacktrace, btSymbols);
    }

    CFArraySetValueAtIndex(activity.log, (activity.idx % AA_MAX_ENTRIES), entry);
    activity.idx++;

    CFRelease(entry);

    if ((activity.unreadCnt != UINT_MAX) && (++activity.unreadCnt >= 0.9*AA_MAX_ENTRIES))  {
        notify_post(kIOPMAssertionsLogBufferHighWM);
        activity.unreadCnt = UINT_MAX;
    }
}


__private_extern__ void logASLAssertionTypeSummary( kerAssertionType type)
{
    applyToAllAssertionsSync(&gAssertionTypes[type], false, 
                             ^(assertion_t *assertion) {
                             logAssertionEvent(kASummaryLog, assertion);                    
                             });
}

static void printAssertionQualifiersToBuf(assertion_t *assertion, char *aBuf, int bufsize)
{
    if ((assertion->audioin || assertion->audioout || assertion->gps || assertion->baseband
            || assertion->bluetooth || assertion->allowsDeviceRestart || assertion->budgetedActivity) == 0) {
        return;
    }
    snprintf(aBuf, bufsize, "[Qualifiers:");

    if (assertion->audioin) {
        strlcat(aBuf, " AudioIn", bufsize);
    }
    if (assertion->audioout) {
        strlcat(aBuf, " AudioOut", bufsize);
    }
    if (assertion->gps) {
        strlcat(aBuf, " GPS", bufsize);
    }

    if (assertion->baseband) {
        strlcat(aBuf, " Baseband", bufsize);
    }

    if (assertion->bluetooth) {
        strlcat(aBuf, " Bluetooth", bufsize);
    }

    if (assertion->allowsDeviceRestart) {
        strlcat(aBuf, " AllowsDeviceRestart", bufsize);
    }

    if (assertion->budgetedActivity) {
        strlcat(aBuf, " BudgetedActivity", bufsize);
    }

    strlcat(aBuf, "]", bufsize);
}


static void printAggregateAssertionsToBuf(char *aBuf, int bufsize, uint32_t kbits)
{
    size_t printed = 0;

    snprintf(aBuf, bufsize, "[System:");

    if (getAssertionLevel(kPreventIdleType)) {
        printed += strlcat(aBuf, " PrevIdle", bufsize);
    }
    if (getAssertionLevel(kPreventDisplaySleepType)) {
        printed += strlcat(aBuf, " PrevDisp", bufsize);
    }
    if (getAssertionLevel(kPreventSleepType)) {
        printed += strlcat(aBuf, " PrevSleep", bufsize);
    }
    if (getAssertionLevel(kDeclareUserActivityType)) {
        printed += strlcat(aBuf, " DeclUser", bufsize);
    }
    if (getAssertionLevel(kPushServiceTaskType)) {
        printed += strlcat(aBuf, " PushSrvc", bufsize);
    }
    if (getAssertionLevel(kBackgroundTaskType)) {
        printed += strlcat(aBuf, " BGTask", bufsize);
    }
    if (getAssertionLevel(kDeclareSystemActivityType)) {
        printed += strlcat(aBuf, " SysAct", bufsize);
    }
    if (getAssertionLevel(kSRPreventSleepType)) {
        printed += strlcat(aBuf, " SRPrevSleep", bufsize);
    }
    if (getAssertionLevel(kTicklessDisplayWakeType)) {
        printed += strlcat(aBuf, " DispWake", bufsize);
    }
    if (getAssertionLevel(kIntPreventDisplaySleepType)) {
        printed += strlcat(aBuf, " IntPrevDisp", bufsize);
    }
    if (getAssertionLevel(kNetworkAccessType)) {
        printed += strlcat(aBuf, " NetAcc", bufsize);
    }
    if (getAssertionLevel(kInteractivePushServiceType)) {
        printed += strlcat(aBuf, " IPushSrvc", bufsize);
    }
    if (kbits & kIOPMDriverAssertionCPUBit) {
        printed += strlcat(aBuf, " kCPU", bufsize);
    }
    if (kbits & kIOPMDriverAssertionPreventDisplaySleepBit) {
        printed += strlcat(aBuf, " kDisp", bufsize);
    }

    if (0 == printed) {
        strlcat(aBuf, " No Assertions", bufsize);
    }

    strlcat(aBuf, "]", bufsize);

    return;
}


static void logAssertionToASL(assertLogAction  action,
                              assertion_t     *assertion)
{
    const int       kLongStringLen          = 200;
    const int       kShortStringLen         = 10;
    CFStringRef     foundAssertionType      = NULL;
    CFStringRef     foundAssertionName      = NULL;
    CFDateRef       foundDate               = NULL;
    CFStringRef     procName                = NULL;
    char            proc_name_buf[kProcNameBufLen];
    char            assertionTypeCString[kLongStringLen];
    char            assertionNameCString[kLongStringLen];
    char            ageString[kShortStringLen];
    char            aslMessageString[kLongStringLen];
    char            assertionsBuf[kLongStringLen];
    char            aslAssertionId[kLongStringLen];
    char            assertionQualifierBuf[kLongStringLen];
    CFMutableDictionaryRef assertionDictionary;
    char            *assertionAction = NULL;
    assertionType_t         *assertType = NULL;


    assertionTypeCString[0] = assertionNameCString[0] =
        aslAssertionId[0] = proc_name_buf[0] = aslMessageString[0] =  ageString[0] = 0;
    if (assertion->state & kAssertionSkipLogging) return;

    if (!(gDebugFlags & kIOPMDebugLogAssertionSynchronous)) {

        assertType = &gAssertionTypes[assertion->kassert];
        if ((action == kACreateLog)  || (action == kATurnOnLog))
        {
            /*
             * Log on create
             *   - if this assertion type has to be logged on create, or
             *   - if display is asleep
             */
            if (!(assertType->flags & kAssertionTypeLogOnCreate))
                return;
            assertion->state |= kAssertionStateLogged;
        }
        else if ( (action == kAReleaseLog) || (action == kAClientDeathLog) ||
                  (action == kATimeoutLog) || (action == kATurnOffLog) )
        {
            uint64_t    delay;

            if (isDisplayAsleep()) {
                delay = DISPLAY_OFF_ASSERTION_LOG_DELAY;
            }
            else {
                delay = DISPLAY_ON_ASSERTION_LOG_DELAY;
            }
            /* 
             * Log on release
             *   - if they are logged when created, or
             *   - if they are released after delay secs 
             */
            if (!(assertion->state & kAssertionStateLogged) &&
                (getMonotonicTime() - assertion->createTime < delay))
                return;

        }
    }

    switch(action) {
    case kACreateLog:
        assertionAction = kPMASLAssertionActionCreate;
        break;
    case kATurnOnLog:
        assertionAction = kPMASLAssertionActionTurnOn;
        break;
    case kAReleaseLog:
        assertionAction = kPMASLAssertionActionRelease;
        break;
    case kAClientDeathLog:
        assertionAction = kPMASLAssertionActionClientDeath;
        break;
    case kATimeoutLog:
        assertionAction = kPMASLAssertionActionTimeOut;
        break;
    case kACapExpiryLog:
        assertionAction = kPMASlAssertionActionCapTimeOut;
        break;
    case kATurnOffLog:
        assertionAction = kPMASLAssertionActionTurnOff;
        break;
    case kASummaryLog:
        assertionAction = kPMASLAssertionActionSummary;
        break;
    case kANameChangeLog:
        if (!(gDebugFlags & kIOPMDebugLogAssertionNameChange)) {
            return;
        }
        assertionAction = kPMASLAssertionActionNameChange;
        break;
    default:
        return;

    }

    assertionDictionary = assertion->props;
    if (assertionDictionary)
    {
        /* 
         * Log the assertion type:
         */
        foundAssertionType = CFDictionaryGetValue(assertionDictionary, kIOPMAssertionTypeKey);        
        if (foundAssertionType) {
            CFStringGetCString(foundAssertionType, assertionTypeCString, 
                               sizeof(assertionTypeCString), kCFStringEncodingUTF8);
        }

        foundAssertionName = CFDictionaryGetValue(assertionDictionary, kIOPMAssertionNameKey);
        if (foundAssertionName) {
            CFStringGetCString(foundAssertionName, assertionNameCString, 
                               sizeof(assertionNameCString), kCFStringEncodingUTF8);            
        }

        /*
         * Assertion's age
         */
        if ((foundDate = CFDictionaryGetValue(assertionDictionary, kIOPMAssertionCreateDateKey)))
        {
            CFAbsoluteTime createdCFTime    = CFDateGetAbsoluteTime(foundDate);
            int createdSince                = (int)(CFAbsoluteTimeGetCurrent() - createdCFTime);
            int hours                       = createdSince / 3600;
            int minutes                     = (createdSince / 60) % 60;
            int seconds                     = createdSince % 60;
            snprintf(ageString, sizeof(ageString), "%02d:%02d:%02d ", hours, minutes, seconds);
        }


        if ((procName = CFDictionaryGetValue(assertionDictionary, kIOPMAssertionProcessNameKey)))
        {
            CFStringGetCString(procName, proc_name_buf, sizeof(proc_name_buf), kCFStringEncodingUTF8);
        }
    }
    printAggregateAssertionsToBuf(assertionsBuf, sizeof(assertionsBuf), getKerAssertionBits());
    snprintf(aslMessageString, sizeof(aslMessageString), "%s", assertionsBuf);

    snprintf(aslAssertionId, sizeof(aslAssertionId), "0x%llx", (((uint64_t)assertion->kassert) << 32) | (assertion->assertionId));

    assertionQualifierBuf[0] = 0;
    printAssertionQualifiersToBuf(assertion, assertionQualifierBuf, sizeof(assertionQualifierBuf));

    if (gDebugFlags & kIOPMDebugAssertionASLLog) {
        char  pid_buf[kShortStringLen];
        aslmsg m = new_msg_pmset_log();
        asl_set(m, kPMASLAssertionTypeKey, assertionTypeCString);
        asl_set(m, kPMASLAssertionNameKey, assertionNameCString);
        asl_set(m, kPMASLAssertionAgeKey, ageString);

        if (1 != assertion->retainCnt)
        {
            char    retainCountBuf[kShortStringLen];
            snprintf(retainCountBuf, sizeof(retainCountBuf), "%d", assertion->retainCnt);
            asl_set(m, "RetainCount", retainCountBuf);
        }


        asl_set(m, kPMASLProcessNameKey, proc_name_buf);
        pid_buf[0] = 0;
        if (0 < snprintf(pid_buf, kShortStringLen, "%d", assertion->pinfo->pid)) {
            asl_set(m, kPMASLPIDKey, pid_buf);
        }

        asl_set(m, kPMASLAssertionIdKey, aslAssertionId );
        asl_set(m, ASL_KEY_MSG, aslMessageString);
        asl_set(m, kPMASLActionKey, assertionAction);
        asl_set(m, kPMASLDomainKey, kPMASLDomainPMAssertions);
        asl_send(NULL, m);
        asl_free(m);
    }

    if (gDebugFlags & kIOPMDebugLogAssertionActivity) {
        INFO_LOG("Process %{public}@.%d %{public}s %{public}@ \"%{public}@\" age:%{public}s id:%lld %{public}s %{public}s",
                procName, assertion->pinfo->pid, assertionAction, foundAssertionType, foundAssertionName, ageString,
                (((uint64_t)assertion->kassert) << 32) | (assertion->assertionId), assertionsBuf, assertionQualifierBuf);
    }

    if (isA_installEnvironment()) {
        syslog(LOG_INFO | LOG_INSTALL, "Assertion %s. Type:%s Name:\'%s\' Id:%s Process:%s %s\n",
                assertionAction, assertionTypeCString, assertionNameCString, aslAssertionId, proc_name_buf, aslMessageString);
    }
}


void logASLAssertionsAggregate( )
{
    char            aslMessageString[100];
    char            assertionsBuf[100];
    char            capacityBuf[64];
    static int      prevPwrSrc = -1;
    static uint32_t prevAssertionBits = 0;
    PowerSources    pwrSrc;
    uint32_t        capacity;
    bool            battExists;

    battExists = getPowerState(&pwrSrc, &capacity);
    pwrSrc = _getPowerSource();
    if ( (prevPwrSrc == pwrSrc) && (prevAssertionBits == getKerAssertionBits()) )
        return;

    prevPwrSrc = pwrSrc;
    prevAssertionBits = getKerAssertionBits();

    printAggregateAssertionsToBuf(assertionsBuf, sizeof(assertionsBuf), getKerAssertionBits());


    if (battExists)
        snprintf(capacityBuf, sizeof(capacityBuf), "(Charge: %d)", capacity);
    else
        capacityBuf[0] = 0;

    snprintf(aslMessageString, sizeof(aslMessageString), "Summary- %s Using %s%s",
             assertionsBuf,
             (pwrSrc == kBatteryPowered) ? "Batt" : "AC",
             capacityBuf);

    if (gDebugFlags & kIOPMDebugAssertionASLLog) {
        aslmsg m = new_msg_pmset_log();
        asl_set(m, ASL_KEY_MSG, aslMessageString);
        asl_set(m, kPMASLActionKey, kPMASLAssertionActionSummary);
        asl_set(m, kPMASLDomainKey, kPMASLDomainPMAssertions);
        asl_send(NULL, m);
        asl_free(m);
    }

    //
    //    for (int i=0; i<kIOPMNumAssertionTypes; i++) {
    //        logASLAssertionTypeSummary(gAssertionTypes[i].kassert);
    //    }
    if (gDebugFlags & kIOPMDebugLogAssertionActivity) {
        INFO_LOG("Summary- %{public}s\n", assertionsBuf);
    }
}

void logASLAllAssertions( )
{
    static dispatch_source_t periodicLogger = 0;

    if (periodicLogger == 0) {

        periodicLogger = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(periodicLogger, ^{
            logASLAllAssertions();
        });

        dispatch_source_set_timer(periodicLogger, dispatch_time(DISPATCH_TIME_NOW, PERIODIC_LOG_INTERVAL * NSEC_PER_SEC),
                DISPATCH_TIME_FOREVER, 0);
        dispatch_resume(periodicLogger);
    }

    for (int i=0; i<kIOPMNumAssertionTypes; i++) {
        if (i == kEnableIdleType) {
            continue;
        }
        logASLAssertionTypeSummary(gAssertionTypes[i].kassert);
    }

    dispatch_source_set_timer(periodicLogger, dispatch_time(DISPATCH_TIME_NOW, PERIODIC_LOG_INTERVAL * NSEC_PER_SEC),
            DISPATCH_TIME_FOREVER, 0);
}


void logAssertionEvent(assertLogAction  action,
                          assertion_t     *assertion)
{

    if (gDebugFlags & (kIOPMDebugAssertionASLLog|kIOPMDebugLogAssertionActivity))
        logAssertionToASL(action, assertion);

    if (gActivityLogCnt)
        logAssertionActivity(action, assertion);
        
}

void setAssertionActivityLog(int value)
{
    // Enabled by default on embedded
    if (value)
        gActivityLogCnt++;
    else if (gActivityLogCnt)
        gActivityLogCnt--;
}

kern_return_t _io_pm_assertion_activity_log (
                                             mach_port_t         server __unused,
                                             audit_token_t       token,
                                             vm_offset_t         *log,
                                             mach_msg_type_number_t   *logSize,
                                             uint32_t                 *refCnt,
                                             uint32_t                 *overflow,
                                             int                      *rc)
{
    CFRange             range;
    CFIndex             startIdx, endIdx;
    CFDataRef           serializedLog = NULL;
    uint32_t            readFromIdx;
    uint32_t            writeToIdx;
    CFMutableArrayRef   updates = NULL;
    CFIndex             arrCnt;
    static bool         firstcall = true;

    if ((log == NULL) || (overflow == NULL))
    {
        *rc = kIOReturnBadArgument;
        goto exit;
    }

 
    *rc = kIOReturnNotFound;
    *log = 0;
    *logSize = 0;
    readFromIdx = *refCnt;
    writeToIdx = activity.idx;
    *overflow = false;

    if (auditTokenHasEntitlement(token, CFSTR("com.apple.private.iokit.powerlogging"))) 
    {
        activity.unreadCnt = 0;
        if (firstcall) {
            *overflow = true;
            *refCnt = readFromIdx = UINT_MAX;
            firstcall = false;
        }
    }

    if (!activity.log) {
        activity.log = CFArrayCreateMutable(NULL, AA_MAX_ENTRIES, &kCFTypeArrayCallBacks);

        if (!activity.log) {
            *rc = kIOReturnNoMemory;
            goto exit;
        }
    }
    arrCnt = CFArrayGetCount(activity.log);

    if ((readFromIdx == writeToIdx) || (arrCnt == 0)) {
        goto exit;
    }
    if ((readFromIdx % AA_MAX_ENTRIES) >= arrCnt) {
        // can happen in case of powerd crash and client provides old refCnt
        *overflow = true;
        asl_log(0,0,ASL_LEVEL_ERR, "Unexpected readFromIdx %d. arrCnt=%ld\n", readFromIdx, arrCnt);
        *refCnt = readFromIdx = UINT_MAX;
    }

    if ((readFromIdx == UINT_MAX) && (writeToIdx <= AA_MAX_ENTRIES)) {
        startIdx = endIdx = 0;
        if (isA_CFArray(activity.log))  {
            endIdx = arrCnt;
        }
        if (endIdx == 0) {
            goto exit; // No entries
        }
        endIdx -= 1;
    }
    else if ((writeToIdx > readFromIdx + AA_MAX_ENTRIES) || (writeToIdx < readFromIdx)) {
        startIdx = writeToIdx % AA_MAX_ENTRIES;
        endIdx = (writeToIdx -1) % AA_MAX_ENTRIES;
        *overflow = true;
        if (startIdx >= arrCnt) {
            // can happen in case of powerd crash and client provides old refCnt
            startIdx = 0;
        }
    }
    else {
        startIdx = readFromIdx % AA_MAX_ENTRIES;
        endIdx = (writeToIdx -1) % AA_MAX_ENTRIES;
    }

    updates = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (updates == NULL) {
        goto exit;
    }

    // Copy log entries in sequential order 
    if (startIdx > endIdx) {
        if (arrCnt == AA_MAX_ENTRIES) {
            range = CFRangeMake(startIdx, AA_MAX_ENTRIES-startIdx);
            CFArrayAppendArray(updates, activity.log, range);
        }
        else {
            asl_log(0,0,ASL_LEVEL_ERR, "arrCnt is %ld. expected %d\n", arrCnt, AA_MAX_ENTRIES);
            asl_log(0,0,ASL_LEVEL_ERR, "startIdx: %ld endIdx: %ld refCnt: 0x%x readFromIdx: 0x%x writeToIdx: 0x%x\n",
                    startIdx, endIdx, *refCnt, readFromIdx, writeToIdx);
            *overflow = true;
        }
        startIdx = 0;
    }

    if (arrCnt >= endIdx+1) {
        range = CFRangeMake(startIdx, endIdx-startIdx+1);
        CFArrayAppendArray(updates, activity.log, range);
    }
    else {
        asl_log(0,0,ASL_LEVEL_ERR, "final: arrCnt is %ld. expected >= %ld\n", arrCnt, endIdx+1);
        *overflow = true;
        *refCnt = activity.idx;
        goto exit;
    }


    serializedLog = CFPropertyListCreateData(0, updates,
                                             kCFPropertyListBinaryFormat_v1_0, 0, NULL);            

    if (!serializedLog)
        goto exit;

    *logSize = (mach_msg_type_number_t)CFDataGetLength(serializedLog);
    vm_allocate(mach_task_self(), (vm_address_t *)log, *logSize, TRUE);
    if (*log == 0)
        goto exit;

    memcpy((void *)*log, CFDataGetBytePtr(serializedLog), *logSize);
    *rc = kIOReturnSuccess;
    
    *refCnt = activity.idx; // Number of entries saved since last reset

exit:
    if (serializedLog)
        CFRelease(serializedLog);

    if (updates)
        CFRelease(updates);
    return KERN_SUCCESS;
}


struct aggregateStats {
    CFMutableDataRef    reportBufs;    /* IOReporter's simple array buffers for each process */
    uint32_t            bufSize;       /* Memory size allocated for reportBufs */
    uint64_t            curTime;

    CFMutableDictionaryRef  legend;     /* Legend for all channels. Each process has a channel */
};

// This will be moved to IOReportTypes.h later
#undef IOREPORT_MAKECHTYPE
#define IOREPORT_NELEMENTSSHIFT 32                                                                                                  
#define IOREPORT_NELEMENTSMASK 0x0000ffff00000000                                                                                   
#define IOREPORT_MAKECHTYPE(format, categories, nelems) \
    ((((uint64_t)(nelems)) << IOREPORT_NELEMENTSSHIFT) | ((format) & 0xff | (uint32_t)(categories) << 16))                                  
#define kIOPMStatsGroup CFSTR("I/O Kit Power Management")
#define kIOPMAssertionsSub CFSTR("Power Assertions")
                                                                                                                                 
void updateProcAssertionStats(ProcessInfo *pinfo, struct aggregateStats *aggStats)
{
    void        *ptr2cpy = NULL;
    uint32_t    size2cpy = 0;
    uint64_t    duration = 0;
    uint64_t    chType = 0;
    IOReturn    ret;


    effectStats_t           *stats = NULL;
    static CFStringRef      providerName = NULL;
    static CFMutableDictionaryRef  unitInfo = NULL;

    if (pinfo->reportBuf == NULL) return;

    if (aggStats->reportBufs == NULL) {
        aggStats->reportBufs = CFDataCreateMutable(NULL, 0);
        if (!aggStats->reportBufs) return;
    }

    if (providerName == NULL) {
        providerName = IOReportCopyCurrentProcessName();
        if (providerName == NULL) return;
    }

    if (unitInfo == NULL) {
        IOReportUnit unit = kIOReportUnit_s;
        unitInfo = CFDictionaryCreateMutable(NULL, 1, 
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        if (!unitInfo) return;

        CFNumberRef unitNum = CFNumberCreate(NULL, kCFNumberLongLongType, &unit);
        if (!unitNum)   return;
        CFDictionarySetValue(unitInfo, CFSTR(kIOReportLegendUnitKey), unitNum);
        CFRelease(unitNum);
    }

    if (aggStats->legend == NULL) {
        aggStats->legend = IOReportCreateAggregate(0);
        if (aggStats->legend == NULL) return;
    }

    chType = IOREPORT_MAKECHTYPE(kIOReportFormatSimpleArray, kIOReportCategoryPower, kMaxEffectStats);
    ret = IOReportAddChannelDescription(aggStats->legend, getpid(), 
                                        providerName, pinfo->pid,
                                        chType, CFSTR("Assertion duration by process"),
                                        kIOPMStatsGroup, kIOPMAssertionsSub,
                                        unitInfo, NULL);
    if (ret != kIOReturnSuccess) 
        return;


    for (kerAssertionEffect i = kNoEffect; i < kMaxEffectStats; i++) {
        stats = &pinfo->stats[i];
        if (stats->cnt) {
            duration = aggStats->curTime - stats->startTime;
            SIMPLEARRAY_INCREMENTVALUE(pinfo->reportBuf, i, duration);
        }
        stats->startTime = aggStats->curTime;
    }

    SIMPLEARRAY_UPDATEPREP(pinfo->reportBuf, ptr2cpy, size2cpy);
    CFDataAppendBytes(aggStats->reportBufs, ptr2cpy, size2cpy);
}

int qcompare(const void *p1, const void*p2)
{
            const ProcessInfo *proc1 = *((ProcessInfo **)p1);
            const ProcessInfo *proc2 = *((ProcessInfo **)p2);

            if (proc1->create_seq < proc2->create_seq) return -1;
            if (proc1->create_seq == proc2->create_seq) return 0;
            return 1;


}

IOReturn copyAssertionActivityAggregate(CFDictionaryRef *data)
{
    ProcessInfo             **procs = NULL;
    CFMutableDictionaryRef  samples = NULL;
    struct aggregateStats   aggStats;
    IOReturn rc;
    CFIndex                 j, cnt;



    memset(&aggStats, 0, sizeof(aggStats));

    if (gActivityAggCnt == 0) {
        asl_log(0,0,ASL_LEVEL_ERR,
                "gActivityAggCnt = 0; IOPMCopyAssertionActivityAggregate()"
                " called without w/o IOPMSetAssertionActivityAggregate(true)?!\n");
        rc = kIOReturnNotOpen;
        goto exit;
    }
    aggStats.curTime = getMonotonicTime();

    cnt = CFDictionaryGetCount(gProcessDict);
    procs = malloc(cnt*(sizeof(ProcessInfo *)));
    if (!procs)  {
        rc = kIOReturnNoMemory;
        goto exit;
    }

    memset(procs, 0, cnt*(sizeof(procs)));
    CFDictionaryGetKeysAndValues(gProcessDict, NULL, (const void **)procs);

    // Sort this array of ProcessInfo structs to return in same order every time.
    // This is to overcome the limitation in IOReporting(see 16270424)
    qsort(procs, cnt, sizeof(procs), qcompare);

    for (j = 0; (j < cnt) && (procs[j]); j++) {
        updateProcAssertionStats(procs[j], &aggStats);
    }

    samples = IOReportCreateSamplesRaw(aggStats.legend, aggStats.reportBufs, NULL);
    free(procs);

    *data = samples;
    rc = kIOReturnSuccess;
exit:
    if (aggStats.legend) {
        CFRelease(aggStats.legend);
    }
    if (aggStats.reportBufs) {
        CFRelease(aggStats.reportBufs);
    }
    return rc;

}

/*
 * This MIG call will return
 * CFArrayRef  procNames : Array of process names for which stats are accumulated
 * void   *reportBufs    : IOReporter's simple array buffers for each process.
 *
 * This data has to be fed to IOReporter APIs to get stats per process.
 * Each IOReporter simple array has stats in the below order:
 *    [0]: kNoEffect
 *    [1]: kPrevIdleSlpEffect
 *    [2]: kPrevDemandSlpEffect
 *    [3]: kPrevDisplaySlpEffect
 */
kern_return_t _io_pm_assertion_activity_aggregate (
                                             mach_port_t         server __unused,
                                             audit_token_t       token,
                                             vm_offset_t         *statsData,
                                             mach_msg_type_number_t   *statsSize,
                                             int                      *rc)
{
    CFDataRef               serializedArray = NULL;
    CFDictionaryRef         samples = NULL;

    *statsSize = 0;
    *rc = copyAssertionActivityAggregate(&samples);
    if (*rc != kIOReturnSuccess) {
        goto exit;
    }


    if (samples == 0) {
        /* No data collected */
        return KERN_SUCCESS;
    }

    serializedArray = CFPropertyListCreateData(0, samples,
                                              kCFPropertyListBinaryFormat_v1_0, 0, NULL);

    if (serializedArray) {
        *statsSize = (mach_msg_type_number_t)CFDataGetLength(serializedArray);
        vm_allocate(mach_task_self(), (vm_address_t *)statsData, *statsSize, TRUE);
        memcpy((void*)*statsData, CFDataGetBytePtr(serializedArray), *statsSize);

    }
    else  {
        *rc = kIOReturnInternalError;
        goto exit;
    }

exit:
    if (samples)
        CFRelease(samples);
    if (serializedArray)
        CFRelease(serializedArray);


    return KERN_SUCCESS;
}
