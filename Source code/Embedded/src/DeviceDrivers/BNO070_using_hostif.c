/*
 * NBO070.c
 *
 * Created: 10/30/2014 3:04:17 PM
 *  Author: Sensics Boger
 */

// Options header
#include "GlobalOptions.h"

// internal headers for BNO driver
#include "BNO070.h"
#include "bno_callbacks.h"

// application headers
#include "my_hardware.h"
#include "TimingDebug.h"
#include "Console.h"
#include "Boot.h"
#include "VideoInput.h"  // for video mode status to be fed into BNO report

// asf headers
#include <ioport.h>
#include <delay.h>  // to dynamically define F_CPU for <util/delay.h>
#include <util/delay.h>
#include <udi_hid_generic.h>
#include <twi_master.h>

// standard headers
#include <string.h>

/* Define this to enable the calibrated gyro reports and stuff them in the USB reports at offset 10 */
#define REPORT_GYRO 1
#define REPORT_ACC 0
#define REPORT_MAG 0

// Skip version check and always DFU
//#define FORCE_DFU 1
/// @todo Without a definition of FORCE_DFU, build fails when defining PERFORM_BNO_DFU.
/// This was added as a fallback.
#ifndef FORCE_DFU
#define FORCE_DFU 0
#endif

#define STABILITY_ON_TABLE 1
#define DCD_SAVE_PERIOD_SEC (300UL)  // 300sec = 5 min

#ifdef BNO070

#include "sensorhub.h"
extern sensorhub_t sensorhub;

bool BNO070Active = false;
uint8_t BNO070_Report[USB_REPORT_SIZE];
bool TWI_BNO070_PORT_initialized = false;  // true if already initialized
uint8_t BNOReportVersion;                  // version 1 or 3 depending on whether velocity is being reported

enum
{
	BNO_USE_RV = 0,
	BNO_USE_GRV = 1
};
/* Set this to 1 to report GRV instead of standard RV */
uint8_t SELECT_GRV =
    BNO_USE_GRV;  // When set to 1, the system will use Game Rotation Vector (GRV) which does not align to
                  // magnetic North.  When set to 0, the system will use Rotation Vector (RV) which will always
                  // try to have its 0 degree heading aligned with magnetic North.

sensorhub_ProductID_t BNO070id;
Bool BNO_supports_400Hz = false;  // true if firmware is new enough to support higher-rate reads

struct BNO070_Config
{
	/* per-sensor configs */
	struct
	{
		sensorhub_SensorFeature_t rv;
		sensorhub_SensorFeature_t grv;
		sensorhub_SensorFeature_t acc;
		sensorhub_SensorFeature_t gyro;
		sensorhub_SensorFeature_t mag;
		sensorhub_SensorFeature_t stab_det;
	} sensors;

	/* calibration flags */
	int cal_flags;

	/* auto DCD save enable */
	int dcd_auto_save;
	uint32_t dcd_save_period;
};

static struct BNO070_Config config_;
static struct BNO070_Config dcdSaveConfig_;
static int magneticFieldStatus_ = 0xff;
static bool printEvents_ = false;

#ifdef PERFORM_BNO_DFU
#if 1  // 1.7.0
#define DFU_MAJOR 1
#define DFU_MINOR 7
#define DFU_PATCH 0
#include "bno-hostif/1000-3251_1.7.0.390_avr.c"
#else  // 1.2.5
#define DFU_MAJOR 1
#define DFU_MINOR 2
#define DFU_PATCH 5
#include "bno-hostif/1000-3251_1.2.5_avr.c"
#endif
#endif

#define DEG2RAD (3.1415926535897932384626433832795 / 180.0)

static int32_t round32(float f) { return (int32_t)(f + ((f >= 0) ? 0.5 : -0.5)); }
static int32_t degToRadQ28(float deg) { return (int32_t)(deg * DEG2RAD * (1ul << 28)); }
static inline int32_t hz2us(int hz)
{
	if (hz == 0)
	{
		return 0;
	}
	return 1000000 / hz;
}

#define toFixed32(x, Q) round32(x *(float)(1ul << Q))

static sensorhub_ProductID_t readProductId(void)
{
	sensorhub_ProductID_t id;
	memset(&id, 0, sizeof(id));

	sensorhub.debugPrintf("Requesting product ID...\r\n");
	int rc = sensorhub_getProductID(&sensorhub, &id);
	if (rc != SENSORHUB_STATUS_SUCCESS)
	{
		sensorhub.debugPrintf("readProductId received error: %d\r\n", rc);
		return id;
	}

	sensorhub.debugPrintf("  Version %d.%d.%d\r\n", id.swVersionMajor, id.swVersionMinor, id.swVersionPatch);
	sensorhub.debugPrintf("  Part number: %d\r\n", id.swPartNumber);
	sensorhub.debugPrintf("  Build number: %d\r\n", id.swBuildNumber);

	return id;
}

static void checkDfu(void)
{
#ifdef PERFORM_BNO_DFU
	sensorhub_ProductID_t id = readProductId();
	if (FORCE_DFU || (id.swVersionMajor != DFU_MAJOR) || (id.swVersionMinor != DFU_MINOR) ||
	    (id.swVersionPatch != DFU_PATCH))
	{
		sensorhub.debugPrintf("BNO is not at %d.%d.%d.  Performing DFU . . . \r\n", DFU_MAJOR, DFU_MINOR, DFU_PATCH);

		int rc = sensorhub_dfu_avr(&sensorhub, &dfuStream);
		if (rc != SENSORHUB_STATUS_SUCCESS)
		{
			sensorhub.debugPrintf("dfu received error: %d\r\n", rc);
			return;
		}
		sensorhub.debugPrintf("DFU Completed Successfully\r\n");
		// Re-probe:
		sensorhub_probe(&sensorhub);

		// Get the updated version number
		readProductId();
	}
#endif  // PERFORM_BNO_DFU
}

// TODO - skip the write FRS if the FRS on the device does not differ
static void configureARVRStabilizationFRS(void)
{
	int status;
	int32_t arvrConfig[4] = {
	    toFixed32(0.2f, 30),  // scaling
	    degToRadQ28(7.3f),    // maxRotation
	    degToRadQ28(90.0f),   // maxError
	    degToRadQ28(0.0f),    // stability
	};

	sensorhub.debugPrintf("Configuring ARVR Stabilization.\r\n");
	status = sensorhub_writeFRS(&sensorhub, SENSORHUB_FRS_ARVR_CONFIG, (uint32_t *)arvrConfig, 4);
	if (status != SENSORHUB_STATUS_SUCCESS)
	{
		sensorhub.debugPrintf("Write FRS failed: %d", status);
	}

	sensorhub.debugPrintf("Configuring ARVR Game Stabilization.\r\n");
	status = sensorhub_writeFRS(&sensorhub, SENSORHUB_FRS_ARVR_GAME_CONFIG, (uint32_t *)arvrConfig, 4);
	if (status != SENSORHUB_STATUS_SUCCESS)
	{
		sensorhub.debugPrintf("Write FRS failed: %d", status);
	}
}

uint8_t const scd[] = {
//#include "bno-hostif/SCD-Bosch-BNO070-8G-lowGyroNoise.c"
//#include "bno-hostif/SCD-Bosch-BNO070-8G.c"
#include "bno-hostif/SCD-Bosch-BNO070-sqtsNoise.c"
};

// TODO - skip the write FRS if the FRS on the device does not differ
static void configureScdFrs(void)
{
	int status;
	sensorhub.debugPrintf("Configuring SCD.\r\n");
	status =
	    sensorhub_writeFRS(&sensorhub, SENSORHUB_FRS_SCD_ACTIVE, (uint32_t const *)scd, sizeof(scd) / sizeof(uint32_t));
	if (status != SENSORHUB_STATUS_SUCCESS)
	{
		sensorhub.debugPrintf("Write FRS of SCD failed: %d", status);
	}
}

static void loadDefaultConfig(struct BNO070_Config *cfg)
{
	int32_t common_period;
	int32_t gyro_period;
	memset(cfg, 0x00, sizeof(*cfg));

	if (BNO_supports_400Hz)
	{
		common_period = hz2us(400);
		gyro_period = hz2us(400);
		cfg->dcd_save_period = (400UL * DCD_SAVE_PERIOD_SEC);
	}
	else
	{
		common_period = hz2us(200);
		gyro_period = 0;
		cfg->dcd_save_period = (200UL * DCD_SAVE_PERIOD_SEC);
	}

	/* disable auto DCD saves */
	cfg->dcd_auto_save = 0;

	/* enable stability detector */
	cfg->sensors.stab_det.reportInterval = hz2us(10);

	/* rv and grv are mutually exclusive */
	cfg->sensors.rv.reportInterval = !SELECT_GRV ? common_period : 0;
	cfg->sensors.grv.reportInterval = SELECT_GRV ? common_period : 0;
	/* enable the other reports as needed */
	cfg->sensors.gyro.reportInterval = REPORT_GYRO ? gyro_period : 0;
	cfg->sensors.acc.reportInterval = REPORT_ACC ? common_period : 0;
	cfg->sensors.mag.reportInterval = REPORT_MAG ? hz2us(100) : 0;

	cfg->cal_flags = ACCEL_CAL_EN;
}

static void loadDcdSaveConfig(struct BNO070_Config *cfg)
{
	int32_t common_period;
	int32_t gyro_period;
	memset(cfg, 0x00, sizeof(*cfg));

	if (BNO_supports_400Hz)
	{
		common_period = hz2us(1);
		gyro_period = hz2us(1);
	}
	else
	{
		common_period = hz2us(1);
		gyro_period = 0;
	}

	/* disable auto DCD saves */
	cfg->dcd_auto_save = 0;

	/* enable stability detector */
	cfg->sensors.stab_det.reportInterval = hz2us(10);

	/* rv and grv are mutually exclusive */
	cfg->sensors.rv.reportInterval = !SELECT_GRV ? common_period : 0;
	cfg->sensors.grv.reportInterval = SELECT_GRV ? common_period : 0;
	/* enable the other reports as needed */
	cfg->sensors.gyro.reportInterval = REPORT_GYRO ? gyro_period : 0;
	cfg->sensors.acc.reportInterval = REPORT_ACC ? common_period : 0;
	cfg->sensors.mag.reportInterval = REPORT_MAG ? hz2us(1) : 0;

	cfg->cal_flags = ACCEL_CAL_EN;
}

static void printEvent(const sensorhub_Event_t *event)
{
	switch (event->sensor)
	{
	case SENSORHUB_ACCELEROMETER:
	{
		sensorhub.debugPrintf("Acc:%02x  0x%04x 0x%04x 0x%04x\r\n", (int)event->sequenceNumber,
		                      event->un.accelerometer.x_16Q8, event->un.accelerometer.y_16Q8,
		                      event->un.accelerometer.z_16Q8);
	}
	break;

	case SENSORHUB_GYROSCOPE_CALIBRATED:
	{
		sensorhub.debugPrintf("Rot:%02x  0x%04x 0x%04x 0x%04x\r\n", (int)event->sequenceNumber,
		                      event->un.gyroscope.x_16Q9, event->un.gyroscope.y_16Q9, event->un.gyroscope.z_16Q9);
	}
	break;

	case SENSORHUB_MAGNETIC_FIELD_CALIBRATED:
	{
		sensorhub.debugPrintf("Mag:%02x  0x%04x 0x%04x 0x%04x\r\n", (int)event->sequenceNumber,
		                      event->un.magneticField.x_16Q4, event->un.magneticField.y_16Q4,
		                      event->un.magneticField.z_16Q4);
	}
	break;

	case SENSORHUB_ROTATION_VECTOR:
	{
		sensorhub.debugPrintf("RV: %02x %5d %5d %5d %5d\r\n", (int)event->sequenceNumber,
		                      event->un.rotationVector.i_16Q14, event->un.rotationVector.j_16Q14,
		                      event->un.rotationVector.k_16Q14, event->un.rotationVector.real_16Q14);
	}
	break;

	case SENSORHUB_GAME_ROTATION_VECTOR:
	{
		sensorhub.debugPrintf("RV: %02x %5d %5d %5d %5d\r\n", (int)event->sequenceNumber,
		                      event->un.gameRotationVector.i_16Q14, event->un.gameRotationVector.j_16Q14,
		                      event->un.gameRotationVector.k_16Q14, event->un.gameRotationVector.real_16Q14);
	}
	break;
	}
}

static void handleEvent(const sensorhub_Event_t *event)
{
	switch (event->sensor)
	{
	case SENSORHUB_ROTATION_VECTOR:
	{
		BNO070_Report[1] = event->sequenceNumber;
		memcpy(&BNO070_Report[2], &event->un.rotationVector.i_16Q14, 8);  // copy quaternion data
#ifdef MeasurePerformance
		TimingDebug_event2();
		TimingDebug_RecordEventType(1);
#endif
		udi_hid_generic_send_report_in(BNO070_Report);
	}
	break;

	case SENSORHUB_GAME_ROTATION_VECTOR:
	{
		BNO070_Report[1] = event->sequenceNumber;
		memcpy(&BNO070_Report[2], &event->un.gameRotationVector.i_16Q14, 8);  // copy quaternion data
#ifdef MeasurePerformance
		TimingDebug_event2();
		TimingDebug_RecordEventType(2);
#endif
		udi_hid_generic_send_report_in(BNO070_Report);
	}
	break;

	case SENSORHUB_GYROSCOPE_CALIBRATED:
	{
		memcpy(&BNO070_Report[10], &event->un.gyroscope.x_16Q9, 6);  // copy gyroscope values
	}
	break;

	case SENSORHUB_MAGNETIC_FIELD_CALIBRATED:
	{
		// store the mag status field only if the mag is enabled
		if (config_.sensors.mag.reportInterval)
		{
			magneticFieldStatus_ = event->status & 0x3;
		}
		// we don't send it to the host (yet?)
	}
	break;
	}

	if (printEvents_)
	{
		printEvent(event);
	}
}

static inline int checkError(int status, const char *msg)
{
	if (status < 0)
	{
		sensorhub.debugPrintf("Err %d: %s", status, msg);
	}
	return status;
}

/**
 * Apply settings in activeConfig_ to BNO
 */
static bool applyConfig(struct BNO070_Config *cfg)
{
	int status;

	/* Disable DCD Auto save, which is on by default */
	status = sensorhub_dcdAutoSave(&sensorhub, cfg->dcd_auto_save);
	checkError(status, "Error configuring DCD auto save.");

	if (BNO_supports_400Hz)
	{
		/* Cal Enable introduced in version 1.8.x */
		status = sensorhub_calEnable(&sensorhub, cfg->cal_flags);
		if (checkError(status, "error setting cal enable flags") < 0)
		{
			return false;
		}
	}

	/* Enable stability classifier -- we need to use it for manual DCD saves */
	status = sensorhub_setDynamicFeature(&sensorhub, SENSORHUB_ACTIVITY_CLASSIFICATION, &cfg->sensors.stab_det);
	if (checkError(status, "error setting Stability Detector") < 0)
	{
		return false;
	}

	status = sensorhub_setDynamicFeature(&sensorhub, SENSORHUB_ROTATION_VECTOR, &cfg->sensors.rv);
	if (checkError(status, "error setting RV") < 0)
	{
		return false;
	}

	status = sensorhub_setDynamicFeature(&sensorhub, SENSORHUB_GAME_ROTATION_VECTOR, &cfg->sensors.grv);
	if (checkError(status, "error setting GRV") < 0)
	{
		return false;
	}

	status = sensorhub_setDynamicFeature(&sensorhub, SENSORHUB_ACCELEROMETER, &cfg->sensors.acc);
	if (checkError(status, "error setting ACCEL") < 0)
	{
		return false;
	}

	status = sensorhub_setDynamicFeature(&sensorhub, SENSORHUB_GYROSCOPE_CALIBRATED, &cfg->sensors.gyro);
	if (checkError(status, "error setting GYRO") < 0)
	{
		return false;
	}

	status = sensorhub_setDynamicFeature(&sensorhub, SENSORHUB_MAGNETIC_FIELD_CALIBRATED, &cfg->sensors.mag);
	if (checkError(status, "error setting MAG") < 0)
	{
		return false;
	}

	return true;
}

bool init_BNO070(void)
{
	int result;

	// determine if we are in GRV or RV mode
	GetValidConfigValueOrWriteDefault(GRVOffset, BNO_USE_GRV, &SELECT_GRV);

	// Clear BNO070_Report so we don't send garbage out the USB.
	memset(BNO070_Report, 0, sizeof(BNO070_Report));

	// reset line is an output but we config as input when deasserted.  External pullup sets in high state.
	// When Reset_Pin must be asserted (low), the pin is reconfigured as an output.
	// This is so that a JTAG debugger can assert reset on the BNO070 if necessary.
	ioport_configure_pin(BNO_070_Reset_Pin, IOPORT_DIR_INPUT);

	ioport_configure_pin(BNO_BOOTN, IOPORT_DIR_OUTPUT | IOPORT_INIT_HIGH);  // Actually the BootN pin
	ioport_configure_pin(Int_BNO070, IOPORT_DIR_INPUT | IOPORT_MODE_PULLUP | IOPORT_FALLING);

	if (!TWI_BNO070_PORT_initialized)
	{
		twi_master_options_t opt_BNO070;

		opt_BNO070.speed = BNO_TWI_SPEED;
		opt_BNO070.chip = BNO070_ADDR;
		result = twi_master_setup(TWI_BNO070_PORT, &opt_BNO070);
		if (result == STATUS_OK)
		{
			TWI_BNO070_PORT_initialized = true;
			// WriteLn("Init success");
		}
		else
			return false;
	}

#ifdef PERFORM_BNO_DFU
	return dfu_BNO070();
#endif

	// Enable interrupts from BNO070.
	PORTD.INTCTRL &= ~PORT_INT0LVL0_bm;  // disable interrupt
	PORTD.INT0MASK = 0x08;               // set interrupt mask for BNO INTN
	PORTD.INTFLAGS = 0xFF;               // clear all pending interrupt flags.
	PORTD.INTCTRL |= PORT_INT0LVL0_bm;   // enable interrupt at high priority.

	// sensorhub_probe must be called before any other sensorhub API calls.
	if (sensorhub_probe(&sensorhub) != SENSORHUB_STATUS_SUCCESS)
	{
		return false;
	}

	// Get Product Id and determine whether 400Hz is supported.
	BNO070id = readProductId();
	if ((BNO070id.swVersionMajor * 10 + BNO070id.swVersionMinor) >= 18)  // version>1.8
		BNO_supports_400Hz = true;

// setup USB output report

// determine report version coming out of BNO
#ifdef REPORT_GYRO
	if (BNO_supports_400Hz)
		BNOReportVersion = 3;
	else
		BNOReportVersion = 1;
#else
	BNOReportVersion = 1;
#endif
	Update_BNO_Report_Header();  // set up some initial value for BNO_report[0]
	BNO070_Report[1] = 0;        // this indicates the sequence number

	// restore normal setting
	configureARVRStabilizationFRS();
	configureScdFrs();

	// reset + probe again after applying FRS settings
	if (sensorhub_probe(&sensorhub) != SENSORHUB_STATUS_SUCCESS)
	{
		return false;
	}

	// configure BNO with our default settings and sensor rate
	ReInit_BNO070();

	return true;
}

bool Check_BNO070(void)
{
#define MAX_EVENTS_AT_A_TIME 1
	sensorhub_Event_t shEvents[MAX_EVENTS_AT_A_TIME];
	int numEvents = 0;
	static uint32_t untilDcdSave = 0xFFFFFFFF;
	int i;
	int rc;

	/* Get the shEvents - we may get 0 */
	rc = sensorhub_poll(&sensorhub, shEvents, MAX_EVENTS_AT_A_TIME, &numEvents);

	if (rc == SENSORHUB_STATUS_HUB_RESET)
	{
		/* reset event received */
		sensorhub.debugPrintf("Hub reset event received\r\n");
		applyConfig(&config_);
		untilDcdSave = config_.dcd_save_period;
	}

	for (i = 0; i < numEvents; i++)
	{
		handleEvent(&shEvents[i]);

		if (config_.dcd_save_period > 0)
		{
			/* Check for DCD Save condition */
			if (untilDcdSave > config_.dcd_save_period)
			{
				untilDcdSave = config_.dcd_save_period;
			}
			if (shEvents[i].sensor == SENSORHUB_GAME_ROTATION_VECTOR)
			{
				if (untilDcdSave > 0)
				{
					/* count down on GRV samples until we reach zero.  After that we need a DCD save. */
					untilDcdSave--;
				}
			}

			if (shEvents[i].sensor == SENSORHUB_ACTIVITY_CLASSIFICATION)
			{
				if (shEvents[i].un.field16[0] == STABILITY_ON_TABLE)
				{
					if (untilDcdSave == 0)
					{
						/* We are on table and it's time to save DCD. */
						SaveDcd_BNO070();

						/* Count down to next DCD save */
						untilDcdSave = config_.dcd_save_period;
					}
				}
			}
		}
	}

	return numEvents > 0;
}

bool Tare_BNO070(void)
{
	// execute tare commands

	// In HID parlance, this is a write to the command register, where the command is set output report. The 07 byte
	// is the axes (P2) and the preceding 00 byte is the tare basis.

	const uint8_t tare_now[] = {0x3f, 0x03, 0x05, 0x00, 0x2F, 0x03, 0x87, 0x06, 0x00, 0x0D, 0x00,
	                            0x00, 0x03, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	twi_package_t packet_write = {
	    .addr[0] = 5,                     // regNum,      // TWI slave memory address data
	    .addr[1] = 0,                     // regNum,      // TWI slave memory address data
	    .addr_length = sizeof(uint16_t),  // TWI slave memory address data size
	    .chip = BNO070_ADDR,              // TWI slave bus address
	    .buffer = (void *)tare_now,       // transfer data source buffer
	    .length = sizeof(tare_now)        // transfer data size (bytes)
	};

	if (twi_master_write(TWI_BNO070_PORT, &packet_write) != STATUS_OK)
		return false;

	const uint8_t persist_tare[] = {0x3f, 0x03, 0x05, 0x00, 0x2F, 0x03, 0x87, 0x06, 0x00, 0x0D, 0x00,
	                                0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	packet_write.addr[0] = 5;                     // regNum,      // TWI slave memory address data
	packet_write.addr[1] = 0;                     // regNum,      // TWI slave memory address data
	packet_write.addr_length = sizeof(uint16_t);  // TWI slave memory address data size
	packet_write.chip = BNO070_ADDR;              // TWI slave bus address
	packet_write.buffer = (void *)persist_tare;   // transfer data source buffer
	packet_write.length = sizeof(persist_tare);   // transfer data size (bytes)

	if (twi_master_write(TWI_BNO070_PORT, &packet_write) != STATUS_OK)
		return false;

	WriteLn("Tare completed");
	return true;
}

bool SetDcdEn_BNO070(uint8_t flags)
{
	config_.cal_flags = flags;
	return applyConfig(&config_);
}

bool SaveDcd_BNO070(void)
{
	/* change sensor rates to 1Hz while we save DCD */
	applyConfig(&dcdSaveConfig_);

	/* save DCD */
	int status = sensorhub_saveDcd(&sensorhub);

	/* restore desired sensor rates now */
	applyConfig(&config_);

	return (status == SENSORHUB_STATUS_SUCCESS);
}

bool ClearDcd_BNO070(void)
{
	/* clear DCD */
	int status = sensorhub_writeFRS(&sensorhub, SENSORHUB_FRS_DCD, NULL, 0);

	return (status == SENSORHUB_STATUS_SUCCESS);
}

bool MagSetEnable_BNO070(bool enabled)
{
	config_.sensors.mag.reportInterval = enabled ? hz2us(25) : 0;
	if (!enabled)
	{
		magneticFieldStatus_ = 0xff;
	}
	return applyConfig(&config_);
}

uint8_t MagStatus_BNO070(void) { return magneticFieldStatus_; }
void GetStats_BNO070(BNO070_Stats_t *stats)
{
	stats->resets = sensorhub_resets;
	stats->events = sensorhub_events;
	stats->empty_events = sensorhub_empty_events;
}

void SetDebugPrintEvents_BNO070(bool enabled) { printEvents_ = enabled; }
bool ReInit_BNO070(void)
{
	loadDefaultConfig(&config_);
	loadDcdSaveConfig(&dcdSaveConfig_);

	return applyConfig(&config_);
}

bool Reset_BNO070(void)
{
	sensorhub.setRSTN(&sensorhub, 0);
	sensorhub.delay(&sensorhub, 10);
	sensorhub.setRSTN(&sensorhub, 1);
	return true;
}

bool dfu_BNO070(void)
{
	checkDfu();
	PrepareForSoftwareUpgrade();
	return true;
}

void Update_BNO_Report_Header()
// update message header to reflect video status

{
	if (BNOReportVersion == 3)
	{
		BNO070_Report[0] = BNOReportVersion + (HDMIStatus << 4);
	}
}

uint8_t Get_BNO_Report_Header()  // for debug, returns first byte of BNO message header
{
	return BNO070_Report[0];
}

#ifdef OSVRHDK
void BNO_Yield()
{
#ifdef BNO070
	{
		if (BNO070Active)
		{
			if (bno_data_ready > 0)
			{
				Check_BNO070();
			}
		}
	}
#endif
}
#endif
#endif
