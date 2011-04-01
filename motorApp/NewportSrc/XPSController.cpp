/*
FILENAME...     XPSMotorDriver.cpp
USAGE...        Newport XPS EPICS asyn motor device driver

Version:        $Revision: 19717 $
Modified By:    $Author: sluiter $
Last Modified:  $Date: 2009-12-09 10:21:24 -0600 (Wed, 09 Dec 2009) $
HeadURL:        $URL: https://subversion.xor.aps.anl.gov/synApps/trunk/support/motor/vstub/motorApp/NewportSrc/XPSMotorDriver.cpp $
*/

/*
Original Author: Mark Rivers
*/

/*
Copyright (c) 2005 University of Chicago and the Regents of the University of 
California. All rights reserved.

synApps is distributed subject to the following license conditions:
SOFTWARE LICENSE AGREEMENT
Software: synApps 
Versions: Release 4-5 and higher.

   1. The "Software", below, refers to synApps (in either source code, or 
      binary form and accompanying documentation). Each licensee is addressed 
          as "you" or "Licensee."

   2. The copyright holders shown above and their third-party licensors hereby 
      grant Licensee a royalty-free nonexclusive license, subject to the 
          limitations stated herein and U.S. Government license rights.

   3. You may modify and make a copy or copies of the Software for use within 
      your organization, if you meet the following conditions:
         1. Copies in source code must include the copyright notice and this 
                    Software License Agreement.
         2. Copies in binary form must include the copyright notice and this 
                    Software License Agreement in the documentation and/or other 
                        materials provided with the copy.

   4. You may modify a copy or copies of the Software or any portion of it, thus
      forming a work based on the Software, and distribute copies of such work
      outside your organization, if you meet all of the following conditions:
         1. Copies in source code must include the copyright notice and this 
                    Software License Agreement;
         2. Copies in binary form must include the copyright notice and this 
                    Software License Agreement in the documentation and/or other 
                        materials provided with the copy;
         3. Modified copies and works based on the Software must carry 
                    prominent notices stating that you changed specified portions of 
                        the Software.

   5. Portions of the Software resulted from work developed under a 
      U.S. Government contract and are subject to the following license: 
          the Government is granted for itself and others acting on its behalf a 
          paid-up, nonexclusive, irrevocable worldwide license in this computer 
          software to reproduce, prepare derivative works, and perform publicly and 
          display publicly.

   6. WARRANTY DISCLAIMER. THE SOFTWARE IS SUPPLIED "AS IS" WITHOUT WARRANTY OF 
      ANY KIND. THE COPYRIGHT HOLDERS, THEIR THIRD PARTY LICENSORS, THE UNITED 
          STATES, THE UNITED STATES DEPARTMENT OF ENERGY, AND THEIR EMPLOYEES: (1) 
          DISCLAIM ANY WARRANTIES, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO 
          ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR 
          PURPOSE, TITLE OR NON-INFRINGEMENT, (2) DO NOT ASSUME ANY LEGAL LIABILITY 
          OR RESPONSIBILITY FOR THE ACCURACY, COMPLETENESS, OR USEFULNESS OF THE 
          SOFTWARE, (3) DO NOT REPRESENT THAT USE OF THE SOFTWARE WOULD NOT 
          INFRINGE PRIVATELY OWNED RIGHTS, (4) DO NOT WARRANT THAT THE SOFTWARE WILL 
          FUNCTION UNINTERRUPTED, THAT IT IS ERROR-FREE OR THAT ANY ERRORS WILL BE 
          CORRECTED.

   7. LIMITATION OF LIABILITY. IN NO EVENT WILL THE COPYRIGHT HOLDERS, THEIR 
      THIRD PARTY LICENSORS, THE UNITED STATES, THE UNITED STATES DEPARTMENT OF 
          ENERGY, OR THEIR EMPLOYEES: BE LIABLE FOR ANY INDIRECT, INCIDENTAL, 
          CONSEQUENTIAL, SPECIAL OR PUNITIVE DAMAGES OF ANY KIND OR NATURE, 
          INCLUDING BUT NOT LIMITED TO LOSS OF PROFITS OR LOSS OF DATA, FOR ANY 
          REASON WHATSOEVER, WHETHER SUCH LIABILITY IS ASSERTED ON THE BASIS OF 
          CONTRACT, TORT (INCLUDING NEGLIGENCE OR STRICT LIABILITY), OR OTHERWISE, 
          EVEN IF ANY OF SAID PARTIES HAS BEEN WARNED OF THE POSSIBILITY OF SUCH 
          LOSS OR DAMAGES.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsExport.h>
#include <epicsString.h>
#include <iocsh.h>

#include "XPSController.h"
#include "XPS_C8_drivers.h"

static const char *driverName = "XPSMotorDriver";

typedef enum { none, positionMove, velocityMove, homeReverseMove, homeForwardsMove } moveType;

/** Struct for a list of strings describing the different corrector types possible on the XPS.*/
typedef struct {
  char *PIPosition;
  char *PIDFFVelocity;
  char *PIDFFAcceleration;
  char *PIDDualFFVoltage;
  char *NoCorrector;
} CorrectorTypes_t;

const static CorrectorTypes_t CorrectorTypes = { 
  "PositionerCorrectorPIPosition",
  "PositionerCorrectorPIDFFVelocity",
  "PositionerCorrectorPIDFFAcceleration",
  "PositionerCorrectorPIDDualFFVoltage",
  "NoCorrector"
};

/** This is controlled via the XPSEnableSetPosition function (available via the IOC shell). */ 
static int doSetPosition = 1;

/**
 * Parameter to control the sleep time used when setting position. 
 * A function called XPSSetPosSleepTime(int) (millisec parameter) 
 * is available in the IOC shell to control this.
 */
static double setPosSleepTime = 0.5;

/** Deadband to use for the velocity comparison with zero. */
#define XPS_VELOCITY_DEADBAND 0.0000001

#define XPS_MAX_AXES 8
#define XPSC8_END_OF_RUN_MINUS  0x80000100
#define XPSC8_END_OF_RUN_PLUS   0x80000200

#define TCP_TIMEOUT 2.0
#define MAX(a,b) ((a)>(b)? (a): (b))
#define MIN(a,b) ((a)<(b)? (a): (b))

XPSController::XPSController(const char *portName, const char *IPAddress, int IPPort, 
                                       int numAxes, double movingPollPeriod, double idlePollPeriod)
  :  asynMotorController(portName, numAxes, NUM_XPS_PARAMS, 
                         asynInt32Mask | asynFloat64Mask | asynUInt32DigitalMask, 
                         asynInt32Mask | asynFloat64Mask | asynUInt32DigitalMask,
                         ASYN_CANBLOCK | ASYN_MULTIDEVICE, 
                         1, // autoconnect
                         0, 0)  // Default priority and stack size
{
  static const char *functionName = "XPSController";
  
  IPAddress_ = epicsStrDup(IPAddress);
  IPPort_ = IPPort;

  // Create controller-specific parameters
  createParam(XPSMinJerkString, asynParamFloat64, &XPSMinJerk_);
  createParam(XPSMaxJerkString, asynParamFloat64, &XPSMaxJerk_);
  createParam(XPSStatusString,  asynParamInt32,   &XPSStatus_);

  pollSocket_ = TCP_ConnectToServer((char *)IPAddress, IPPort, TCP_TIMEOUT);
  if (pollSocket_ < 0) {
    printf("%s:%s: error calling TCP_ConnectToServer for pollSocket\n",
           driverName, functionName);
  }

  FirmwareVersionGet(pollSocket_, firmwareVersion_);
  
  /* Create the poller thread for this controller
   * NOTE: at this point the axis objects don't yet exist, but the poller tolerates this */
  startPoller(movingPollPeriod, idlePollPeriod, 10);
}

void XPSController::report(FILE *fp, int level)
{
  int axis;
  XPSAxis *pAxis;

  fprintf(fp, "XPS motor driver %s, numAxes=%d, firmware version=%s, moving poll period=%f, idle poll period=%f\n", 
          this->portName, numAxes_, firmwareVersion_, movingPollPeriod_, idlePollPeriod_);

  if (level > 0) {
    for (axis=0; axis<numAxes_; axis++) {
      pAxis = getAxis(axis);
      fprintf(fp, "  axis %d\n"
                  "    name = %s\n"
                  "    step size = %g\n"
                  "    poll socket = %d, moveSocket = %d\n"
                  "    status = %d\n", 
              pAxis->axisNo_,
              pAxis->positionerName_, 
              pAxis->stepSize_, 
              pAxis->pollSocket_, pAxis->moveSocket_, 
              pAxis->axisStatus_);
    }
  }

  // Call the base class method
  asynMotorController::report(fp, level);
}

/**
 * Perform a deferred move (a coordinated group move) on all the axes in a group.
 * @param pController Pointer to XPSController structure.
 * @param groupName Pointer to string naming the group on which to perform the group move.
 * @return motor driver status code.
 */
asynStatus XPSController::processDeferredMovesInGroup(char *groupName)
{
  double positions[XPS_MAX_AXES];
  int positions_index = 0;
  int first_loop = 1;
  int axis = 0;
  int NbPositioners = 0;
  int relativeMove = 0;
  int status = 0;
  XPSAxis *pAxis=NULL;

  /* Loop over all axes in this controller. */
  for (axis=0; axis<numAxes_; axis++) {
    pAxis = getAxis(axis);
    
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "Executing deferred move on XPS: %s, Group: %s\n", 
              this->portName, groupName);

    /* Ignore axes in other groups. */
    if (!strcmp(pAxis->groupName_, groupName)) {
      if (first_loop) {
        /* Get the number of axes in this group. */
        NbPositioners = pAxis->isInGroup();
        first_loop = 0;
      }

      /* Set relative flag for the actual move at the end of the funtion.*/
      if (pAxis->deferredRelative_) {
        relativeMove = 1;
      }

      /* Build position buffer.*/
      if (pAxis->deferredMove_) {
        positions[positions_index] = 
          pAxis->deferredRelative_ ? (pAxis->setpointPosition_ + pAxis->deferredPosition_) : pAxis->deferredPosition_;
      } else {
        positions[positions_index] = 
          pAxis->deferredRelative_ ? 0 : pAxis->setpointPosition_;
      }

      /* Reset deferred flag. */
      /* We need to do this for the XPS, because we cannot do partial group moves. Every axis
         in the group will be included the next time we do a group move. */
      pAxis->deferredMove_ = 0;

      /* Next axis in this group. */
      positions_index++;
    }
  }
  
  /* Send the group move command. */
  if (relativeMove) {
    status = GroupMoveRelative(pAxis->moveSocket_,
                               groupName,
                               NbPositioners,
                               positions);
  } else {
    status = GroupMoveAbsolute(pAxis->moveSocket_,
                               groupName,
                               NbPositioners,
                               positions);
  }
  
  if (status!=0) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
              "Error peforming GroupMoveAbsolute/Relative in processDeferredMovesInGroup. XPS Return code: %d\n", 
              status);
    return asynError;
  }  

  return asynSuccess;
  
}

/**
 * Process deferred moves for a controller and groups.
 * This function calculates which unique groups in the controller
 * and passes the controller pointer and group name to processDeferredMovesInGroup.
 * @return motor driver status code.
 */
asynStatus XPSController::processDeferredMoves()
{
  asynStatus status = asynError;
  int axis = 0;
  int i = 0;
  int dealWith = 0;
  /* Array to cache up to XPS_MAX_AXES group names. Don't initialise to null */
  char *groupNames[XPS_MAX_AXES];
  char *blankGroupName = " ";
  XPSAxis *pAxis;

  /* Clear group name cache. */
  for (i=0; i<XPS_MAX_AXES; i++) {
    groupNames[i] = blankGroupName;
  }

  /* Loop over axes, testing for unique groups. */
  for (axis=0; axis<numAxes_; axis++) {
    pAxis = getAxis(axis);
  
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "Processing deferred moves on XPS: %s\n", this->portName);
  
    /* Call processDeferredMovesInGroup only once for each group on this controller.
       Positioners in the same group may not be adjacent in list, so we have to test for this. */
    for (i=0; i<XPS_MAX_AXES; i++) {
      if (strcmp(pAxis->groupName_, groupNames[i])) {
        dealWith++;
        groupNames[i] = pAxis->groupName_;
      }
    }
    if (dealWith == XPS_MAX_AXES) {
      dealWith = 0;
      /* Group name was not in cache, so deal with this group. */
      status = this->processDeferredMovesInGroup(pAxis->groupName_);
    }
    /*  Next axis, and potentially next group.*/
  }
  
  return status;
}

asynStatus XPSController::writeFloat64(asynUser *pasynUser, epicsFloat64 value)
{
  int function = pasynUser->reason;
  int status = asynSuccess;
  XPSAxis *pAxis = this->getAxis(pasynUser);
  double deviceValue;
  static const char *functionName = "writeFloat64";
  
  /* Set the parameter and readback in the parameter library. */
  status = pAxis->setDoubleParam(function, value);
  
  if (function == motorResolution_)
  {
    pAxis->stepSize_ = value;
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "%s:%s: Set XPS %s, axis %d stepSize to %f\n", 
               driverName, functionName, portName, pAxis->axisNo_, value);
  }
  else if (function == motorLowLimit_)
  {
    deviceValue = value*pAxis->stepSize_;
    /* We need to read the current highLimit because otherwise we could be setting it to an invalid value */
    status = PositionerUserTravelLimitsGet(pAxis->pollSocket_,
                                           pAxis->positionerName_,
                                           &pAxis->lowLimit_, &pAxis->highLimit_);
    if (status != 0) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s:%s: motorAxisSetDouble[%s,%d]: error performing PositionerUserTravelLimitsGet for lowLim status=%d\n",
                driverName, functionName, portName, pAxis->axisNo_, status);
      goto done;
    }
    status = PositionerUserTravelLimitsSet(pAxis->pollSocket_,
                                           pAxis->positionerName_,
                                           deviceValue, pAxis->highLimit_);
    if (status != 0) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s:%s: motorAxisSetDouble[%s,%d]: error performing PositionerUserTravelLimitsSet for lowLim=%f status=%d\n",
                driverName, functionName, portName, pAxis->axisNo_, deviceValue, status);
      goto done;
    } 
    pAxis->lowLimit_ = deviceValue;
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "%s:%s: Set XPS %s, axis %d low limit to %f\n", 
              driverName, functionName, portName, pAxis->axisNo_, deviceValue);
    status = asynSuccess;
  } else if (function == motorHighLimit_)
  {
    deviceValue = value*pAxis->stepSize_;
    /* We need to read the current highLimit because otherwise we could be setting it to an invalid value */
    status = PositionerUserTravelLimitsGet(pAxis->pollSocket_,
                                           pAxis->positionerName_,
                                           &pAxis->lowLimit_, &pAxis->highLimit_);
    if (status != 0) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s:%s: motorAxisSetDouble[%s,%d]: error performing PositionerUserTravelLimitsGet for highLim status=%d\n",
                driverName, functionName, portName, pAxis->axisNo_, status);
      goto done;
    }
    status = PositionerUserTravelLimitsSet(pAxis->pollSocket_,
                                           pAxis->positionerName_,
                                           pAxis->lowLimit_, deviceValue);
    if (status != 0) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s:%s: motorAxisSetDouble[%s,%d]: error performing PositionerUserTravelLimitsSet for highLim=%f status=%d\n",
                driverName, functionName, portName, pAxis->axisNo_, deviceValue, status);
      goto done;
    } 
    pAxis->highLimit_ = deviceValue;
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "%s:%s: Set XPS %s, axis %d high limit to %f\n", 
              driverName, functionName, portName, pAxis->axisNo_, deviceValue);
    status = asynSuccess;
  } else if (function == motorPgain_)
  {
    status = pAxis->setPID(&value, 0);
  } else if (function == motorIgain_)
  {
    status = pAxis->setPID(&value, 1);
  } else if (function == motorDgain_)
  {
    status = pAxis->setPID(&value, 2);
  } else {
    /* Call base class method */
    status = asynMotorController::writeFloat64(pasynUser, value);
  }
  done:
  return (asynStatus)status;
}


asynStatus XPSController::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
  int function = pasynUser->reason;
  int status = asynSuccess;
  XPSAxis *pAxis = this->getAxis(pasynUser);
  static const char *functionName = "writeInt32";

  /* Set the parameter and readback in the parameter library.  This may be overwritten when we read back the
   * status at the end, but that's OK */
  status = pAxis->setIntegerParam(function, value);

  if (function == motorSetClosedLoop_)
  {
    if (value) {
      status = GroupMotionEnable(pAxis->pollSocket_, pAxis->groupName_);
      if (status) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                  "%s:%s: [%s,%d]: error calling GroupMotionEnable status=%d\n",
                   driverName, functionName, portName, pAxis->axisNo_, status);
      } else {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                  "%s:%s: motorAxisSetInteger set XPS %s, axis %d closed loop enable\n",
                   driverName, functionName, portName, pAxis->axisNo_);
      }
    } else {
      status = GroupMotionDisable(pAxis->pollSocket_, pAxis->groupName_);
      if (status) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                  "%s:%s: [%s,%d]: error calling GroupMotionDisable status=%d\n",
                  driverName, functionName, portName, pAxis->axisNo_, status);
      } else {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                  "%s:%s: motorAxisSetInteger set XPS %s, axis %d closed loop disable\n",
                  driverName, functionName, portName, pAxis->axisNo_);
      }
    }
  } else if (function == motorDeferMoves_)
  {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "%s%s: Setting deferred move mode on XPS %s to %d\n", 
               driverName, functionName, portName, value);
    if (value == 0.0 && this->movesDeferred_ != 0) {
      status = this->processDeferredMoves();
    }
    this->movesDeferred_ = value;
    if (status) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s%s: Deferred moved failed on XPS %s, status=%d\n", 
                driverName, functionName, portName, status);
      status = asynError;
    }
  } else {
    /* Call base class method */
    status = asynMotorController::writeInt32(pasynUser, value);
  }
  return (asynStatus)status;
}

XPSAxis* XPSController::getAxis(asynUser *pasynUser)
{
  return static_cast<XPSAxis*>(asynMotorController::getAxis(pasynUser));
}

XPSAxis* XPSController::getAxis(int axisNo)
{
  return static_cast<XPSAxis*>(asynMotorController::getAxis(axisNo));
}



/** The following functions have C linkage, and can be called directly or from iocsh */
extern "C" {
/**
 * Function to enable/disable the write down of position to the 
 * XPS controller. Call this function at IOC shell.
 * @param setPos 0=disable, 1=enable
 */
/* void XPSEnableSetPosition(int setPos) 
{
  doSetPosition = setPos;
}
 */
/**
 * Function to set the threadSleep time used when setting the XPS position.
 * The sleep is performed after the axes are initialised, to take account of any
 * post initialisation wobble.
 * @param posSleep The time in miliseconds to sleep.
 */
/* void XPSSetPosSleepTime(int posSleep) 
{
  setPosSleepTime = (double)posSleep / 1000.0;
}
 */
asynStatus XPSCreate(const char *portName, const char *IPAddress, int IPPort,
                         int numAxes, int movingPollPeriod, int idlePollPeriod)
{
    XPSController *pXPSController
        = new XPSController(portName, IPAddress, IPPort, numAxes, movingPollPeriod/1000., idlePollPeriod/1000.);
    pXPSController = NULL;
    return(asynSuccess);
}

asynStatus XPSCreateAxis(const char *XPSName,         /* specify which controller by port name */
                         int axis,                    /* axis number 0-7 */
                         const char *positionerName,  /* groupName.positionerName e.g. Diffractometer.Phi */
                         int stepsPerUnit)            /* steps per user unit */
{
  XPSController *pC;
  XPSAxis *pAxis;
  static const char *functionName = "XPSCreateAxis";

  pC = (XPSController*) findAsynPortDriver(XPSName);
  if (!pC) {
    printf("%s:%s: Error port %s not found\n",
           driverName, functionName, XPSName);
    return asynError;
  }
  pAxis = new XPSAxis(pC, axis, positionerName, 1./stepsPerUnit);
  pAxis = NULL;
  return(asynSuccess);
}

/* Code for iocsh registration */

/* XPSCreate */
static const iocshArg XPSCreateArg0 = {"Controller port name", iocshArgString};
static const iocshArg XPSCreateArg1 = {"IP address", iocshArgString};
static const iocshArg XPSCreateArg2 = {"IP port", iocshArgInt};
static const iocshArg XPSCreateArg3 = {"Number of axes", iocshArgInt};
static const iocshArg XPSCreateArg4 = {"Moving poll rate (ms)", iocshArgInt};
static const iocshArg XPSCreateArg5 = {"Idle poll rate (ms)", iocshArgInt};
static const iocshArg * const XPSCreateArgs[6] = {&XPSCreateArg0,
                                                  &XPSCreateArg1,
                                                  &XPSCreateArg2,
                                                  &XPSCreateArg2,
                                                  &XPSCreateArg4,
                                                  &XPSCreateArg5};
static const iocshFuncDef createXPS = {"XPSCreate", 6, XPSCreateArgs};
static void createXPSCallFunc(const iocshArgBuf *args)
{
  XPSCreate(args[0].sval, args[1].sval, args[2].ival, 
            args[3].ival, args[4].ival, args[5].ival);
}


/* XPSCreateAxis */
static const iocshArg XPSCreateAxisArg0 = {"Controller port name", iocshArgString};
static const iocshArg XPSCreateAxisArg1 = {"Axis number", iocshArgInt};
static const iocshArg XPSCreateAxisArg2 = {"Axis name", iocshArgString};
static const iocshArg XPSCreateAxisArg3 = {"Steps per unit", iocshArgInt};
static const iocshArg * const XPSCreateAxisArgs[4] = {&XPSCreateAxisArg0,
                                                      &XPSCreateAxisArg1,
                                                      &XPSCreateAxisArg2,
                                                      &XPSCreateAxisArg3};
static const iocshFuncDef createXPSAxis = {"XPSCreateAxis", 4, XPSCreateAxisArgs};

static void createXPSAxisCallFunc(const iocshArgBuf *args)
{
  XPSCreateAxis(args[0].sval, args[1].ival, args[2].sval, args[3].ival);
}


/* void XPSEnableSetPosition(int setPos) */
static const iocshArg XPSEnableSetPositionArg0 = {"Set Position Flag", iocshArgInt};
static const iocshArg * const XPSEnableSetPositionArgs[1] = {&XPSEnableSetPositionArg0};
static const iocshFuncDef xpsEnableSetPosition = {"XPSEnableSetPosition", 1, XPSEnableSetPositionArgs};
static void xpsEnableSetPositionCallFunc(const iocshArgBuf *args)
{
//  XPSEnableSetPosition(args[0].ival);
}

/* void XPSSetPosSleepTime(int posSleep) */
static const iocshArg XPSSetPosSleepTimeArg0 = {"Set Position Sleep Time", iocshArgInt};
static const iocshArg * const XPSSetPosSleepTimeArgs[1] = {&XPSSetPosSleepTimeArg0};
static const iocshFuncDef xpsSetPosSleepTime = {"XPSSetPosSleepTime", 1, XPSSetPosSleepTimeArgs};
static void xpsSetPosSleepTimeCallFunc(const iocshArgBuf *args)
{
//  XPSSetPosSleepTime(args[0].ival);
}


static void XPSRegister3(void)
{
  iocshRegister(&createXPS,            createXPSCallFunc);
  iocshRegister(&createXPSAxis,        createXPSAxisCallFunc);
//  iocshRegister(&xpsEnableSetPosition, xpsEnableSetPositionCallFunc);
//  iocshRegister(&xpsSetPosSleepTime,   xpsSetPosSleepTimeCallFunc);
}

epicsExportRegistrar(XPSRegister3);
} // extern "C"