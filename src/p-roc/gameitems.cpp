#if defined(PINMAME) && defined(PROC_SUPPORT)

extern "C" {
#include "driver.h"
#include "wpc/core.h"
#include "wpc/wpc.h"
#include "wpc/se.h"
#include "wpc/s11.h"
#include "input.h"
}
#include <fstream>
#include <yaml-cpp/yaml.h>
#include "p-roc.h"
#include "p-roc_drivers.h"

// Handle to proc instance
extern PRHandle proc;

bool ignoreCoils[80] = { FALSE };
int coilKickback[256] = { 0 };
int swKickback[256] = { 0 };
int cyclesSinceTransition[256] = { 0 };
int kickbackOnDelay[256];
int kickbackOffDelay[256];
std::vector<int> activeCoils;
CoilDriver coilDrivers [256];
extern PRMachineType machineType;
extern YAML::Node yamlDoc;
extern bool autoPatterDetection;

#define kFlipperLwL          0
#define kFlipperLwR          1
#define kStartButton         2
#define kESQSequence         3
#define numInputCodes        4

#define sign(x) (( x > 0 ) - ( x < 0 ))

int switchStates[kPRSwitchPhysicalLast + 1] = {0};
int swMap[numInputCodes];

int switchEventsBeingProcessed = 0;

void set_swState(int value, int type) {

    	switchStates[value] = type;
	switch (type) {
		case kPREventTypeSwitchOpenDebounced:
		case kPREventTypeSwitchClosedDebounced:
		case kPREventTypeSwitchOpenNondebounced:
		case kPREventTypeSwitchClosedNondebounced:
			if (machineType == kPRMachineSternWhitestar || machineType == kPRMachineSternSAM) {
				// Flippers need to go to column 12 for some reason
				if (value < 12) {	// Dedicated Switches
					int local_value = value;

					// For Whitestar, the flipper switches are used in
					// reverse order in pinmame. Not sure why, but there's
					// code in se.c to do the reversing. So, reverse
					// them as they come in so that they'll be corrected by
					// the reversing code in se.c
					switch (value) {
						case 8:
							local_value=11;
							break;
						case 9:
							local_value=10;
							break;
						case 10:
							local_value=9;
							break;
						case 11:
							local_value=8;
							break;
					}

					int temp = (7-(local_value & 0x7));
					core_setSw(se_m2sw(10, temp), (type & kPREventTypeSwitchClosedDebounced));
				} else if (value < 16) {	// Dedicated Switches
					// Service switches need to go to column -1 for some reason.
					core_setSw(se_m2sw(-1, 7-(value & 0x7)), (type & kPREventTypeSwitchClosedDebounced));
				} else if (value >= 32) {	// Matrix Switches
                                    	core_setSw(se_m2sw(((value - 32) >> 4), value & 0x7), (type & kPREventTypeSwitchClosedDebounced));
				}
			} else if (core_gameData->gen & GEN_ALLS11) {
                                if (mame_debug) fprintf(stderr,"\nS11 PROC switch %d is ",value);
				if (value < 8) {	// Flipper Switches
                                        if (mame_debug) fprintf(stderr,"flipper switch");
					core_setSw(s11_m2sw(CORE_FLIPPERSWCOL, value), (type & kPREventTypeSwitchClosedDebounced));
				} else if (value < 16) {	// Dedicated Switches
				    if (mame_debug) fprintf(stderr,"direct switch");
                                    core_setSw(s11_m2sw(0, value & 0x7), (type & kPREventTypeSwitchClosedDebounced));
				} else if (value >= 32) {	// Matrix Switches
                                        if (mame_debug) fprintf(stderr,"col %d, row %d, event %d",((value - 16) >> 4),(value & 0x7)+1,(type & kPREventTypeSwitchClosedDebounced));
                                    	core_setSw(s11_m2sw(((value - 16) >> 4), value & 0x7), (type & kPREventTypeSwitchClosedDebounced));
				}
			} else {
                                if (mame_debug) fprintf(stderr,"\nWPC PROC switch %d is ",value);
                                //slug
                                //if (value==99 && (type & kPREventTypeSwitchClosedDebounced))
				if (value < 8) {	// Flipper Switches
                                        if(mame_debug) fprintf(stderr,"flipper switch");
					core_setSw(wpc_m2sw(CORE_FLIPPERSWCOL, value), (type & kPREventTypeSwitchClosedDebounced));
				} else if (value < 16) {	// Dedicated Switches
                                        if (mame_debug) fprintf(stderr,"direct switch");
					core_setSw(wpc_m2sw(0, value & 0x7), (type & kPREventTypeSwitchClosedDebounced));
				} else if (value >= 32) {	// Matrix Switches
                                        if (mame_debug) fprintf(stderr,"col %d, row %d, event %d",((value - 16) >> 4),(value & 0x7)+1,(type & kPREventTypeSwitchClosedDebounced));
                                    	core_setSw(wpc_m2sw(((value - 16) >> 4), value & 0x7), (type & kPREventTypeSwitchClosedDebounced));
				}
			}
			break;

		case kPREventTypeDMDFrameDisplayed:
			break;
	}
}

void ConfigureWPCFlipperSwitchRule(int swNum, int mainCoilNum, int holdCoilNum, int pulseTime)
{
	const int numDriverRules = 2;
	PRDriverState fldrivers[numDriverRules];
	PRSwitchRule sw;

	// Flipper on rules
	PRDriverGetState(proc, mainCoilNum, &fldrivers[0]);
	PRDriverStatePulse(&fldrivers[0], pulseTime);	// Pulse coil for 34ms.
	PRDriverGetState(proc, holdCoilNum, &fldrivers[1]);
	PRDriverStatePulse(&fldrivers[1], 0);	// Turn on indefintely (set pulse for 0ms)
	sw.notifyHost = false;
	sw.reloadActive = false;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchClosedNondebounced, &sw, fldrivers, numDriverRules);
	sw.notifyHost = true;
	sw.reloadActive = false;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchClosedDebounced, &sw, NULL, 0);

	// Flipper off rules
	PRDriverGetState(proc, mainCoilNum, &fldrivers[0]);
	PRDriverStateDisable(&fldrivers[0]);	// Disable main coil
	PRDriverGetState(proc, holdCoilNum, &fldrivers[1]);
	PRDriverStateDisable(&fldrivers[1]);	// Disable hold coil
	sw.notifyHost = false;
	sw.reloadActive = false;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchOpenNondebounced, &sw, fldrivers, numDriverRules);
	sw.notifyHost = true;
	sw.reloadActive = false;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchOpenDebounced, &sw, NULL, 0);
}

void ConfigureSternFlipperSwitchRule(int swNum, int mainCoilNum, int pulseTime, int patterOnTime, int patterOffTime)
{
	const int numDriverRules = 1;
	PRDriverState fldrivers[numDriverRules];
	PRSwitchRule sw;

	// Flipper on rules
	PRDriverGetState(proc, mainCoilNum, &fldrivers[0]);
	PRDriverStatePatter(&fldrivers[0], patterOnTime, patterOffTime, pulseTime);	// Pulse coil for 34ms.
	sw.notifyHost = false;
	sw.reloadActive = false;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchClosedNondebounced, &sw, fldrivers, numDriverRules);
	sw.notifyHost = true;
	sw.reloadActive = false;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchClosedDebounced, &sw, NULL, 0);

	// Flipper off rules
	PRDriverGetState(proc, mainCoilNum, &fldrivers[0]);
	PRDriverStateDisable(&fldrivers[0]);	// Disable main coil
	sw.notifyHost = false;
	sw.reloadActive = false;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchOpenNondebounced, &sw, fldrivers, numDriverRules);
	sw.notifyHost = true;
	sw.reloadActive = false;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchOpenDebounced, &sw, NULL, 0);
}

void ConfigureBumperRule(int swNum, int coilNum, int pulseTime)
{
	const int numDriverRules = 1;
	PRDriverState fldrivers[numDriverRules];
	PRSwitchRule sw;
        PRDriverGetState(proc, coilNum, &fldrivers[0]);
	PRDriverStatePulse(&fldrivers[0], pulseTime);	// Pulse coil for 34ms.
	sw.reloadActive = true;
	sw.notifyHost = false;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchClosedNondebounced, &sw, fldrivers, numDriverRules);
	sw.notifyHost = true;
	sw.reloadActive = false;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchClosedDebounced, &sw, NULL, 0);
}

// In addition to the automatic rules for bumpers (above), we need a slightly different
// rule to handle kickbacks.  Specifically we don't want the reloadActive option set,
// since we want to make sure the balls gets hit
void ConfigureKickerRule(int lampNum, int swNum, int coilNum, int pulseTime)
{
	const int numDriverRules = 1;
	PRDriverState fldrivers[numDriverRules];
	PRSwitchRule sw;
        PRDriverGetState(proc, coilNum, &fldrivers[0]);
	PRDriverStatePulse(&fldrivers[0], pulseTime);	
	sw.reloadActive = false;
	sw.notifyHost = true;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchClosedNondebounced, &sw, fldrivers, numDriverRules);
	sw.notifyHost = true;
	sw.reloadActive = false;
	PRSwitchUpdateRule(proc, swNum, kPREventTypeSwitchClosedDebounced, &sw, fldrivers, numDriverRules);
        
}

// Unlike regular bumper rules, for kickbacks we need to be able to clear the
// rule.  Called by procKickbackCheck when the related lamp goes off.
void ClearKickerRule(int lampNum, int swNum)
{
        PRSwitchRule sw;
        sw.notifyHost = true;
        sw.reloadActive = false;
        PRSwitchUpdateRule(proc,swNum,kPREventTypeSwitchClosedNondebounced, &sw, NULL, 0);
        sw.notifyHost = true;
        sw.reloadActive = false;
        PRSwitchUpdateRule(proc,swNum,kPREventTypeSwitchClosedDebounced, &sw, NULL, 0);
}

// This routine is called by the lamp matrix handler (in wpc.c and s11.c) for each
// lamp associated with a kickback.
// The general idea is that for each lamp in the matrix which is related to a kickback
// a counter is held of how many cycles (typically each 1/60 second) it's been since the
// lamp last went on or off.  If the number is positive, it's the number of cycles since
// the lamp went on, if negative the number since it went off.
void procKickbackCheck(int num)
{
    // If this is the first time into this routine for a specific lamp, it's possible
    // that we haven't noted yet if the lamp is currently lit or not.  If so, query the
    // P-ROC to find out.
        if (cyclesSinceTransition[num] == 0) {
            PRDriverState lampState;
            memset(&lampState, 0, sizeof(lampState));
            PRDriverGetState(proc, num, &lampState);
            if (mame_debug) fprintf(stderr,"\nTransition count is zero, need to check P-ROC");
            if (mame_debug) fprintf(stderr,"\nP-ROC reports that lamp state is %s",lampState.state ? "on" : "off");
            cyclesSinceTransition[num] = (lampState.state ? 1 : -1);
        }

        // If we've reached the cycle count required to turn the related kickback off
        if (cyclesSinceTransition[num] == kickbackOffDelay[num]) {
            if (mame_debug) fprintf(stderr,"\nDisable kickback");
            ClearKickerRule(num,swKickback[num]);
        }
        // If we've reached the cycle count required to turn the related kickback on
        else if (cyclesSinceTransition[num] == kickbackOnDelay[num]) {
            if (mame_debug) fprintf(stderr,"\nEnable kickback with pulse time %d",(coilDrivers[coilKickback[num]].GetPulseTime() == 0 ? kKickbackPulseTime : coilDrivers[coilKickback[num]].GetPulseTime()) );

            ConfigureKickerRule(num,swKickback[num], coilKickback[num], (coilDrivers[coilKickback[num]].GetPulseTime() == 0 ? kKickbackPulseTime : coilDrivers[coilKickback[num]].GetPulseTime()));
        }

        // Give the cycle counter a step in the appropriate direction.
        cyclesSinceTransition[num] += (abs(cyclesSinceTransition[num]) < 5000) * sign(cyclesSinceTransition[num]);
    
}
void AddIgnoreCoil(int num) {
	ignoreCoils[num] = TRUE;
}


void procConfigureDriverDefaults(void)
{
	if (yamlDoc.size() > 0) {
		// WPC Flippers
		std::string numStr;
		const YAML::Node& coils = yamlDoc[kCoilsSection];

		for (YAML::Iterator coilsIt = coils.begin(); coilsIt != coils.end(); ++coilsIt) {
			std::string coilName;
			coilsIt.first() >> coilName;
			int coilNum, pulseTime, patterOnTime, patterOffTime;

			yamlDoc[kCoilsSection][coilName][kNumberField] >> numStr; coilNum = PRDecode(machineType, numStr.c_str());

			// Look for yaml entries defining coil pulse times.
			if (yamlDoc[kCoilsSection][coilName].FindValue(kPulseTimeField)) {
				yamlDoc[kCoilsSection][coilName][kPulseTimeField] >> pulseTime;
				coilDrivers[coilNum].SetPulseTime(pulseTime);
			}

			// Look for yaml entries defining coil patter times.
			if (yamlDoc[kCoilsSection][coilName].FindValue(kPatterOnTimeField) &&
			     yamlDoc[kCoilsSection][coilName].FindValue(kPatterOnTimeField)) {
				yamlDoc[kCoilsSection][coilName][kPatterOnTimeField] >> patterOnTime;
				yamlDoc[kCoilsSection][coilName][kPatterOffTimeField] >> patterOffTime;
				coilDrivers[coilNum].SetPatterTimes(patterOnTime, patterOffTime);
				printf("\nSetting patter times for coil %d: On: %d, Off: %d", coilNum, patterOnTime, patterOffTime);
			}
			if (yamlDoc[kCoilsSection][coilName].FindValue(kBusField)) {
				std::string busStr;
				yamlDoc[kCoilsSection][coilName][kBusField] >> busStr;
				if (busStr.compare(kAuxPortValue) == 0) {
					coilDrivers[coilNum].SetPatterDetectionEnable(0);
				}
			}
		}
	}
}

void procConfigureSwitchRules(void)
{
	if (yamlDoc.size() > 0) {
		// WPC Flippers
		std::string numStr;
                const YAML::Node& flippers = yamlDoc[kFlippersSection];
                for (YAML::Iterator flippersIt = flippers.begin(); flippersIt != flippers.end(); ++flippersIt) {
			int swNum, coilMain, coilHold;
			std::string flipperName;
                        *flippersIt >> flipperName;
			if (machineType == kPRMachineWPC || machineType == kPRMachineWPC95) {
				yamlDoc[kSwitchesSection][flipperName][kNumberField] >> numStr;
				swNum = PRDecode(machineType, numStr.c_str());

				yamlDoc[kCoilsSection][flipperName + "Main"][kNumberField] >> numStr;
				coilMain = PRDecode(machineType, numStr.c_str());

				yamlDoc[kCoilsSection][flipperName + "Hold"][kNumberField] >> numStr;
				coilHold = PRDecode(machineType, numStr.c_str());

				ConfigureWPCFlipperSwitchRule(swNum, coilMain, coilHold, kFlipperPulseTime);
				AddIgnoreCoil(coilMain);
				AddIgnoreCoil(coilHold);
			} else if (machineType == kPRMachineSternWhitestar || machineType == kPRMachineSternSAM) {
				yamlDoc[kSwitchesSection][flipperName][kNumberField] >> numStr;
				swNum = PRDecode(machineType, numStr.c_str());

				yamlDoc[kCoilsSection][flipperName + "Main"][kNumberField] >> numStr;
				coilMain = PRDecode(machineType, numStr.c_str());

				ConfigureSternFlipperSwitchRule(swNum, coilMain, kFlipperPulseTime, kFlipperPatterOnTime, kFlipperPatterOffTime);
				AddIgnoreCoil(coilMain);
			}
		}

                printf("\n\nProcessing bumper entries");
                if (yamlDoc.FindValue(kBumpersSection)) {
                    const YAML::Node& bumpers = yamlDoc[kBumpersSection];
                    for (YAML::Iterator bumpersIt = bumpers.begin(); bumpersIt != bumpers.end(); ++bumpersIt) {
                            int swNum, coilNum;
                            // WPC Slingshots
                            std::string bumperName;
                            *bumpersIt >> bumperName;
                            yamlDoc[kSwitchesSection][bumperName][kNumberField] >> numStr;
                            swNum = PRDecode(machineType, numStr.c_str());

                            yamlDoc[kCoilsSection][bumperName][kNumberField] >> numStr;
                            coilNum = PRDecode(machineType, numStr.c_str());

                            ConfigureBumperRule(swNum, coilNum, kBumperPulseTime);
                            AddIgnoreCoil(coilNum);
                            printf("\n- processed %s",bumperName.c_str());
                    }
                }
                else printf("\n - no entries found");

                // Process kickback entries, if found
                printf("\n\nProcessing kickback entries");
                if (yamlDoc.FindValue(kKickbacksSection)) {
                    const YAML::Node& kickbacks = yamlDoc[kKickbacksSection];
                    for (YAML::Iterator kickbacksIt = kickbacks.begin(); kickbacksIt != kickbacks.end(); ++kickbacksIt) {
                        int swNum=0, coilNum=0, lampNum=0, delayOnTime=kKickbackDelayOnTime, delayOffTime=kKickbackDelayOffTime;
                        std::string kickbackName;
                        kickbacksIt.first() >> kickbackName;


                        // Look for yaml entries defining delay times.

                        if (yamlDoc[kKickbacksSection][kickbackName].FindValue(kKickbackDelayOnField)) {
                            yamlDoc[kKickbacksSection][kickbackName][kKickbackDelayOnField] >> delayOnTime;
                        }
                        if (yamlDoc[kKickbacksSection][kickbackName].FindValue(kKickbackDelayOffField)) {
                            yamlDoc[kKickbacksSection][kickbackName][kKickbackDelayOffField] >> delayOffTime;
                        }
                        yamlDoc[kSwitchesSection][kickbackName][kNumberField] >> numStr;
                        swNum = PRDecode(machineType, numStr.c_str());
                        yamlDoc[kCoilsSection][kickbackName][kNumberField] >> numStr;
                        coilNum = PRDecode(machineType, numStr.c_str());
                        yamlDoc[kLampsSection][kickbackName][kNumberField] >> numStr;
                        lampNum = PRDecode(machineType, numStr.c_str());

                        coreGlobals.isKickbackLamp[lampNum] = TRUE;
                        coilKickback[lampNum] = coilNum;
                        swKickback[lampNum] = swNum;
                        kickbackOnDelay[lampNum] = (delayOnTime *60) / 1000;
                        kickbackOffDelay[lampNum] = ((delayOffTime * 60) / 1000) * -1;

                        // This coil is under P-ROC control, not pinmame, so add it to the ignore list
                        AddIgnoreCoil(coilNum);
                        printf("\n- processed %s",kickbackName.c_str());
                        if (mame_debug) fprintf(stderr,"\nKicker %s is lamp %d, switch %d, coil %d, delay on %d, delay off %d",kickbackName.c_str(),lampNum,swNum,coilNum, delayOnTime, delayOffTime);
                }
            }
                else printf("\n- no entries found");
	}
}

void procSetSwitchStates(void) {
	int i;
	PREventType procSwitchStates[kPRSwitchPhysicalLast + 1];

	// Get all of the switch states from the P-ROC.
	if (PRSwitchGetStates(proc, procSwitchStates, kPRSwitchPhysicalLast + 1) == kPRFailure) {
		fprintf(stderr, "Error: Unable to retrieve switch states from P-ROC.\n");
	} else {
		// Copy the returning states into the local switches array.
		if (mame_debug) fprintf(stderr, "\nInitial switch states:");
		for (i = 0; i <= kPRSwitchPhysicalLast; i++) {
			if (i%16==0) {
				if (mame_debug) fprintf(stderr, "\n");
			}
			if (mame_debug) fprintf(stderr, "%d ", procSwitchStates[i]);

			if (machineType == kPRMachineSternSAM) {
				set_swState(i, procSwitchStates[i]);
			} else if ((i < 32) || ((i%16) < 8)) {
				set_swState(i, procSwitchStates[i]);
			}
		}
		if (mame_debug) fprintf(stderr, "\n\n");
	}
}

void procConfigureDefaultSwitchRules(void) {
	int ii;
	PRSwitchConfig switchConfig;

	// Configure switch controller registers
	switchConfig.clear = FALSE;
	switchConfig.use_column_8 = (machineType == kPRMachineWPC);
	//switchConfig.use_column_8 = FALSE;
	switchConfig.use_column_9 = FALSE;	// No WPC machines actually use this.
	switchConfig.hostEventsEnable = TRUE;
	switchConfig.directMatrixScanLoopTime = 2;	// milliseconds
	switchConfig.pulsesBeforeCheckingRX = 10;
	switchConfig.inactivePulsesAfterBurst = 12;
	switchConfig.pulsesPerBurst = 6;
	switchConfig.pulseHalfPeriodTime = 13;	// milliseconds
	PRSwitchUpdateConfig(proc, &switchConfig);

	//fprintf(stderr, "\nConfiguring P-ROC switch rules");
	// Go through the switches array and reset the current status of each switch
	for (ii = 0; ii <= kPRSwitchPhysicalLast; ii++) {
		PRSwitchRule swRule;
		if ((machineType == kPRMachineWPC || machineType == kPRMachineWPC95) &&
		      !((ii < 32) || ((ii % 16) < 8))) {
			swRule.notifyHost = 0;
		} else {
			swRule.notifyHost = 1;
		}
		PRSwitchUpdateRule(proc, ii, kPREventTypeSwitchClosedNondebounced, &swRule, NULL, 0);
		PRSwitchUpdateRule(proc, ii, kPREventTypeSwitchOpenNondebounced, &swRule, NULL, 0);
	}
}

void procConfigureInputMap(void)
{
    printf ("\n\nProcessing control switches");
	if (yamlDoc.size() > 0) {
                std::string numStr;
                if (yamlDoc[kSwitchesSection].FindValue("flipperLwL")) {
                    yamlDoc[kSwitchesSection]["flipperLwL"][kNumberField] >> numStr;
                    swMap[kFlipperLwL] = PRDecode(machineType, numStr.c_str());
                    printf("\n- FlipperLwL: %d", swMap[kFlipperLwL]);
                }
                if (yamlDoc[kSwitchesSection].FindValue("flipperLwR")) {
                    yamlDoc[kSwitchesSection]["flipperLwR"][kNumberField] >> numStr;
                    swMap[kFlipperLwR] = PRDecode(machineType, numStr.c_str());
                    printf("\n- FlipperLwR: %d", swMap[kFlipperLwR]);
                }
                if (yamlDoc[kSwitchesSection].FindValue("startButton")) {
                    yamlDoc[kSwitchesSection]["startButton"][kNumberField] >> numStr;
                    swMap[kStartButton] = PRDecode(machineType, numStr.c_str());
                    printf("\n- startButton: %d", swMap[kStartButton]);
                }
	}
}

void procDriveLamp(int num, int state) {
	PRDriverState lampState;
        memset(&lampState, 0, sizeof(lampState));
	PRDriverGetState(proc, num, &lampState);
	lampState.state = state;
	lampState.outputDriveTime = PROC_LAMP_DRIVE_TIME;
	lampState.waitForFirstTimeSlot = FALSE;
	lampState.timeslots = 0;
	lampState.patterOnTime = 0;
	lampState.patterOffTime = 0;
	lampState.patterEnable = FALSE;
        PRDriverUpdateState(proc, &lampState);

        cyclesSinceTransition[num] = (state ? 1 : -1);

}

void procGetSwitchEvents(void) {
	int i;
	PREvent eventArray[16];

	switchEventsBeingProcessed = 1;
	int numEvents = PRGetEvents(proc, eventArray, 16);
	for (i = 0; i < numEvents; i++) {
		PREvent *pEvent = &eventArray[i];
		if (pEvent->type == kPREventTypeDMDFrameDisplayed) {
			if (machineType != kPRMachineWPCAlphanumeric) {
				procUpdateDMD();
			}
		}
		else {
                    	set_swState(pEvent->value, pEvent->type);
		}
	}
}

void procGetSwitchEventsLocal(void) {
	int i;
	PREvent eventArray[16];

	int numEvents = PRGetEvents(proc, eventArray, 16);
	for (i = 0; i < numEvents; i++) {
		PREvent *pEvent = &eventArray[i];
		switchStates[pEvent->value] = pEvent->type;
	}
}

CoilDriver::CoilDriver(void) {
	num = 0;
        patterDetectionEnable = 1;
	timeLastChanged = 0;
	numPatterOn = 0;
	numPatterOff = 0;
	avgOnTime = 0;
	avgOffTime = 0;
	patterActive = 0;
        useDefaultPulseTime = 1;
	useDefaultPatterTimes = 1;
}

void CoilDriver::SetNum(int number) {
	num = number;
}

void CoilDriver::SetPulseTime(int ms) {
	pulseTime = ms;
	useDefaultPulseTime = 0;
}

void CoilDriver::SetPatterTimes(int msOn, int msOff) {
	patterOnTime = msOn;
	patterOffTime = msOff;
	useDefaultPatterTimes = 0;
}

void CoilDriver::SetPatterDetectionEnable(int enable) {
	patterDetectionEnable = 0;
}

int CoilDriver::GetPulseTime() {
    return pulseTime;
}

int CoilDriver::GetPatterDetectionEnable() {
	return patterDetectionEnable;
}

void CoilDriver::ResetPatter(void) {
	numPatterOn = 0;
	numPatterOff = 0;
	avgOnTime = 0;
	avgOffTime = 0;
	patterActive = 0;
	reqPatterState = 0;
}

void CoilDriver::CheckEndPatter(void) {
	if (patterActive) {
		// If too much time has passed since the last change, disable the patter.
		if ( ((clock()/CLOCKS_PER_MS) - timeLastChanged) > PROC_MAX_PATTER_INTERVAL_MS) {
			long int msTime = clock() / CLOCKS_PER_MS;
			fprintf (stderr, "\nAt time: %ld: Ending Patter for Coil %d.", msTime, num);
			//Drive(reqPatterState);
			Drive(0);

			ResetPatter();
		}
	}
}

void CoilDriver::Drive(int state) {
	PRDriverState coilState;
	PRDriverGetState(proc, num, &coilState);

	if (num != 68 && num > 39  && mame_debug) {
		long int msTime = clock() / CLOCKS_PER_MS;
		fprintf(stderr,"\nAt time: %ld, Driving %d: %d", msTime, num, state);
	}

	if (state) {
		if (useDefaultPulseTime) {
			PRDriverStatePulse(&coilState, PROC_COIL_DRIVE_TIME);
		} else {
			if (useDefaultPatterTimes) {
				PRDriverStatePulse(&coilState, pulseTime);
			}
			else {
				PRDriverStatePatter(&coilState, patterOnTime, patterOffTime, pulseTime);
			}
		}
	} else {
		// Don't disable if the coil only has a defined pulse
		// time because it should be allowed to expire naturally.
		if (!(!(useDefaultPulseTime) && useDefaultPatterTimes)) {
			PRDriverStateDisable(&coilState);
		}
	}
	PRDriverUpdateState(proc, &coilState);
}

void CoilDriver::Patter(int msOn, int msOff) {
	long int msTime = clock() / CLOCKS_PER_MS;
	fprintf(stderr, "\nAt time: %ld: Setting patter for coil: %d, on:%d, off:%d", msTime, num, msOn, msOff);
	PRDriverState coilState;
	PRDriverGetState(proc, num, &coilState);
	if (useDefaultPatterTimes) {
		PRDriverStatePatter(&coilState, msOn, msOff, 0);
	} else {
		PRDriverStatePatter(&coilState, patterOnTime, patterOffTime, 0);
	}
	PRDriverUpdateState(proc, &coilState);
	patterActive = 1;
}

void CoilDriver::RequestDrive(int state) {
	long int msSinceChanged = (clock() / CLOCKS_PER_MS) - timeLastChanged;
	timeLastChanged = clock() / CLOCKS_PER_MS;

	if ( msSinceChanged > PROC_MAX_PATTER_INTERVAL_MS) {
		ResetPatter();
		Drive(state);
	} else if (patterActive) {
		reqPatterState = state;
	} else {
		if (state) {
			int numPatterOld;
			numPatterOld = numPatterOff++;
			avgOffTime = ((avgOffTime * numPatterOld) + msSinceChanged) / numPatterOff;
			// If Enough transitions have occurred to indicate a patter, enable patter.
			if (patterDetectionEnable && numPatterOff > 2 && autoPatterDetection) {
				Patter(avgOnTime, avgOffTime);
			} else {	// Otherwise drive as requested.
				Drive(state);
			}
		} else {
			int numPatterOld;
			numPatterOld = numPatterOn++;
			avgOnTime = ((avgOnTime * numPatterOld) + msSinceChanged) / numPatterOn;
			// Update patter stats.
			Drive(state);
		}
	}
}

void procDriveCoil(int num, int state) {
    if (mame_debug) {
        if (!((core_gameData->gen & GEN_ALLS11) && num ==63) )
             fprintf(stderr,"\n -procDriveCoil %d %d",num,state);
    }
        
	if (!ignoreCoils[num]) {
		coilDrivers[num].RequestDrive(state);
	}
}

void procCheckActiveCoils(void) {
	int i;
	for (i=0; i<256; i++) {
		coilDrivers[i].CheckEndPatter();
	}
}

void procCheckActiveCoilsUnused(void) {
	std::vector<int> eraseList;

	PRDriverState coilState;
	int timeSinceChanged = 60;

	for (std::vector<int>::iterator it = activeCoils.begin(); it!=activeCoils.end(); ++it) {
		PRDriverGetState(proc, *it, &coilState);
		if (timeSinceChanged > PROC_MAX_PATTER_INTERVAL_MS) {
			procDriveCoil(*it, coilState.state);
			// Don't erase yet because it will change the vector and affect the iterator.
			eraseList.push_back(*it);
			fprintf(stderr, "\nEnding patter for coil %d", *it);
		}
	}

	for (std::vector<int>::iterator it = eraseList.begin(); it!=eraseList.end(); ++it) {
		activeCoils.erase(activeCoils.begin() + *it);
	}
}

static int count;

void procInitializeCoilDrivers(void) {
	int i;
	count = 0;
	for (i=0; i<256; i++) {
		coilDrivers[i].SetNum(i);
	}
}

int isSwitchClosed(int index) {
	return (switchStates[index] == kPREventTypeSwitchClosedNondebounced ||
	        switchStates[index] == kPREventTypeSwitchClosedDebounced);
}

void earlyInputSetup(void) {
	if (proc && !switchEventsBeingProcessed) {
		if (machineType == kPRMachineWPCAlphanumeric || pmoptions.alpha_on_dmd) {
			procDriveLamp(79, 1);
			procTickleWatchdog();
		}
		procGetSwitchEventsLocal();
		procFlush();
	}

}

int osd_is_proc_pressed(int code)
{
	earlyInputSetup();
	switch (code) {
		case kFlipperLwL:
			return (isSwitchClosed(swMap[kFlipperLwL]));
		case kFlipperLwR:
			return (isSwitchClosed(swMap[kFlipperLwR]));
		case kStartButton:
			return (isSwitchClosed(swMap[kStartButton]));
		case kESQSequence:
			return (osd_is_proc_pressed(kFlipperLwL) &&
			        osd_is_proc_pressed(kFlipperLwR) &&
			        osd_is_proc_pressed(kStartButton));
		default:
			return 0;
	}
}

#endif /* PINMAME && PROC_SUPPORT */
