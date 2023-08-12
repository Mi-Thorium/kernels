#include <linux/androidboot_mode.h>
#include <linux/display_state.h>
#include <linux/pmic-voter.h>
#include <linux/power_supply.h>
#include <linux/qpnp/qpnp-adc.h>
#include <nokia-sdm439/mach.h>
#include "smb5-lib.h"

#define smblib_err(chg, fmt, ...)		\
	pr_err("%s: %s: " fmt, chg->name,	\
		__func__, ##__VA_ARGS__)	\

#define USBIN_500MA	500000

#define BATT_TYPE_FCC_VOTER     "BATT_TYPE_FCC_VOTER"
#define BATTCHG_USER_EN_VOTER   "BATTCHG_USER_EN_VOTER"
#define CUST_JEITA_VOTER		"CUST_JEITA_VOTER"

#define BATT_CC_0MA         0
#define BATT_CC_500MA       500000
#define BATT_CC_700MA       700000
#define BATT_CC_800MA       800000
#define BATT_CC_900MA       900000
#define BATT_CC_1000MA      1000000
#define BATT_CC_1100MA      1100000
#define BATT_CC_1200MA      1200000
#define BATT_CC_1400MA      1400000
#define BATT_CC_1800MA      1800000
#define BATT_CC_2000MA      2000000

#define BATT_MAX_VOLTAGE_UNDER_HOT 4100000

static unsigned int battery_cold_temp = 0;
static unsigned int battery_cool_temp = 0;
static unsigned int battery_warm_temp = 0;
static unsigned int battery_hot_temp = 0;
static unsigned int board_good_temp = 0;
static unsigned int board_warm_temp = 0;
static unsigned int board_hot_temp = 0;
static unsigned int batt_cold_fcc = 0;
static unsigned int batt_cool_fcc = 0;
static unsigned int batt_good_fcc = 0;
static unsigned int batt_warm_fcc = 0;
static unsigned int batt_hot_fcc = 0;
static unsigned int board_good_fcc = 0;
static unsigned int board_warm_fcc = 0;
static unsigned int board_hot_fcc = 0;
static unsigned int batt_warm_fv = 0;

#define BATTERY_COLD_TEMP 	battery_cold_temp
#define BATTERY_COOL_TEMP 	battery_cool_temp
#define BATTERY_WARM_TEMP	battery_warm_temp
#define BATTERY_HOT_TEMP	battery_hot_temp
#define BOARD_GOOD_TEMP     board_good_temp
#define BOARD_WARM_TEMP     board_warm_temp
#define BOARD_HOT_TEMP      board_hot_temp
#define BATT_COLD_FCC	batt_cold_fcc
#define BATT_COOL_FCC	batt_cool_fcc
#define BATT_GOOD_FCC	batt_good_fcc
#define BATT_WARM_FCC	batt_warm_fcc
#define BATT_HOT_FCC	batt_hot_fcc
#define BOARD_GOOD_FCC	board_good_fcc
#define BOARD_WARM_FCC	board_warm_fcc
#define BOARD_HOT_FCC	batt_warm_fv
#define BATT_WARM_FV	batt_warm_fv

#define BOARD_TEMP_DEFAULT  25
#define MSM_TEMP_DEFAULT    25

static int usb_source_changed_flag = 0;
static bool powerOnFirstTime = true;

static bool isAndroidBootModeInCharging(void)
{
	return androidboot_mode_get() == ANDROIDBOOT_MODE_CHARGER;
}

static int smblib_get_prop_batt_temp(struct smb_charger *chg,
                              union power_supply_propval *val)
{
	int rc;

	if (!chg->bms_psy)
		return -EINVAL;

	rc = power_supply_get_property(chg->bms_psy,
                                   POWER_SUPPLY_PROP_TEMP, val);

	return rc;
}

static int get_board_temp(struct smb_charger *chip)
{
	int rc;
	struct qpnp_vadc_result board_temp_result;

	if (nokia_sdm439_mach_get() == NOKIA_SDM439_MACH_PANTHER)
		return 30;

	rc = qpnp_vadc_read(chip->nokia_sdm439_board_temp_vadc_dev,
                        VADC_AMUX2_GPIO_PU2, &board_temp_result);
	if (rc) {
		pr_err("[THERMAL] error in board_temp (channel-%d) read rc = %d\n",
               VADC_AMUX2_GPIO_PU2, rc);
		return BOARD_TEMP_DEFAULT;
	}

	return board_temp_result.physical/1000;
}

static int recharge_after_poweron(struct power_supply* batt_psy)
{
	union power_supply_propval prop = {0};

	if(!batt_psy)
		return 0;

	power_supply_get_property(batt_psy, POWER_SUPPLY_PROP_STATUS, &prop);
	if( prop.intval == POWER_SUPPLY_STATUS_FULL) {
		prop.intval = 0;
		power_supply_set_property(batt_psy, POWER_SUPPLY_PROP_RECHARGE_SOC, &prop);
	}

	return 0;
}

/* Function chg_protection_with_thermal for regulating charging current. */
static int chg_protection_with_thermal(struct smb_charger *chip)
{
	int rc = 0;
	int bat_temp, board_temp; /* These are the temperatures of battery and board */
	int bat_current = 0, bat_current1 = 0, bat_current2 = 0;
	int usb_current =0;
	int icl_current =0;
	bool usb_present;
	bool usb_online;
	bool warm_fv_flags = false;
	union power_supply_propval pval = {0};
	int batt_status, charge_type;
	struct power_supply* psy = chip->batt_psy;
	struct power_supply* usb_psy = chip->usb_psy;

	int batt_voltage_now = 0;
	int batt_current_now = 0;
	int usb_current_now  = 0;

	smblib_get_prop_batt_status(chip, &pval);
	batt_status = pval.intval;
	smblib_get_prop_batt_charge_type(chip, &pval);
	charge_type = pval.intval;

	smblib_get_prop_usb_present(chip, &pval);
	usb_present = pval.intval;

	smblib_get_prop_usb_online(chip, &pval);
	usb_online = pval.intval;

	if(!usb_present)
		return 0;

	board_temp = get_board_temp(chip);
	smblib_get_prop_batt_temp(chip, &pval);
	bat_temp = pval.intval/10;

	rc = power_supply_get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
	if (rc >= 0) {
		batt_voltage_now = pval.intval; /* Failure must not happen. */
	} else {
		pr_err("[%s] obtain battery voltage failed : %d \n", __func__, rc);
		batt_voltage_now = BATT_MAX_VOLTAGE_UNDER_HOT;
	            /* This is a default maximum value which a battery
	            can be charged to reach in uv, and it is also under
	            the safety when temperature of battery is at bwtween
	            45 and 60 degrees. */
	}

	power_supply_get_property(psy, POWER_SUPPLY_PROP_CURRENT_NOW, &pval);
	batt_current_now = pval.intval;
	power_supply_get_property(usb_psy, POWER_SUPPLY_PROP_INPUT_CURRENT_NOW, &pval);
	usb_current_now = pval.intval;
	if (bat_temp <= BATTERY_COLD_TEMP) {
		bat_current1 = BATT_COLD_FCC;
		warm_fv_flags = false;
	} else if (BATTERY_COLD_TEMP < bat_temp && bat_temp <= BATTERY_COOL_TEMP) { // 0 - 10
		bat_current1 = BATT_COOL_FCC;
		warm_fv_flags = false;
	} else if (BATTERY_COOL_TEMP < bat_temp && bat_temp < BATTERY_WARM_TEMP) { // 10 - 45
		bat_current1 = BATT_GOOD_FCC;
		warm_fv_flags = false;
	} else if (BATTERY_WARM_TEMP <= bat_temp && bat_temp < BATTERY_HOT_TEMP) { // 45 - 60
		bat_current1 = BATT_WARM_FCC;
//		if(batt_voltage_now >= BATT_MAX_VOLTAGE_UNDER_HOT) // Maximum is limited to 4.1v
//			bat_current1 = BATT_HOT_FCC;
		warm_fv_flags = true;
	} else {
	    /* bat_temp >= 60 */
		bat_current1 = BATT_HOT_FCC;
		warm_fv_flags = true;
	}

	if(warm_fv_flags) {
                chip->batt_profile_fv_uv = BATT_WARM_FV;
                vote(chip->fv_votable, CUST_JEITA_VOTER, true, BATT_WARM_FV);
	} else {
		chip->batt_profile_fv_uv = 4400000;
		vote(chip->fv_votable, CUST_JEITA_VOTER, false, 0);
	}
	bat_current = bat_current1;

	if (nokia_sdm439_mach_get() == NOKIA_SDM439_MACH_DEADPOOL) {
        /* board temperature is no longer needed here for adjusting charging current. */
        if (BATTERY_COOL_TEMP < bat_temp && bat_temp < BATTERY_WARM_TEMP) { // 15 - 45
            if (board_temp < BOARD_GOOD_TEMP) {
                usb_current = BOARD_GOOD_FCC;
            } else if (BOARD_GOOD_TEMP <= board_temp && board_temp < BOARD_WARM_TEMP) {
                usb_current = BOARD_WARM_FCC;
            } else if (BOARD_WARM_TEMP <= board_temp && board_temp < BOARD_HOT_TEMP) {
                usb_current = BOARD_HOT_FCC;
            } else {
                usb_current = BOARD_HOT_FCC;
            }
        } else if (BATTERY_WARM_TEMP <= bat_temp && bat_temp < BATTERY_HOT_TEMP) {// 45 - 50
            usb_current = BOARD_HOT_FCC;
        } else {
            usb_current = BOARD_GOOD_FCC;
        }

        if ((usb_current != chip->nokia_sdm439_last_bat_current) || usb_source_changed_flag) {
            pr_err("[%s] board_temp = %d, bat_temp = %d, usb_current = %d, nokia_sdm439_last_bat_current = %d, \n",
                __func__, board_temp, bat_temp, usb_current, chip->nokia_sdm439_last_bat_current);
            chip->nokia_sdm439_last_usb_current = usb_current;
            vote(chip->usb_icl_votable, SW_ICL_MAX_VOTER, true,
                            usb_current);
            if(rc < 0) {
                pr_err("[%s] Could not vote charger usb_icl_votable \n", __func__);
            }
        }
    } else {
        /* Regulating the battery Fast charging current
        * according to board temperature with lcd status.
        */
        if(is_display_on()) {
            if(0 <= board_temp && board_temp < BOARD_WARM_TEMP) { // 0 - 43
                bat_current2 = BATT_CC_1200MA;
            } else if (BOARD_WARM_TEMP <= board_temp && board_temp < BOARD_HOT_TEMP) { // 43 - 60
                bat_current2 = BATT_CC_900MA;
            } else if (BOARD_HOT_TEMP <= board_temp) { // >= 60
                bat_current2 = BATT_CC_0MA;
            } else { // < 0
                bat_current2 = BATT_CC_1200MA;
            }

            bat_current = min(bat_current1, bat_current2);
        }
    }

	/* This is only for shutdown charging. */
	if(isAndroidBootModeInCharging()) {
		if (chip->real_charger_type == POWER_SUPPLY_TYPE_USB
		&& (chip->typec_legacy
		|| chip->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
		|| chip->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)) {
			smblib_get_prop_input_current_settled(chip, &pval);
			printk("[Shutdown-Charging] settled input current limit : %d \n", pval.intval);
			if(pval.intval < USBIN_500MA){
				pval.intval = USBIN_500MA;
				smblib_set_icl_current(chip, pval.intval);
			}
		}
	}

	/* The protection has been established for a particular
	case which the phone cannot get into charging state,
	with the level of battery power around 95% or beyond
	, would frequently happen. */
	if(powerOnFirstTime){
		recharge_after_poweron(psy);
		powerOnFirstTime = false;
	}

	/* This would only happen when something goes wrong with charing. */
	/*if(charge_type == POWER_SUPPLY_CHARGE_TYPE_NONE   &&
	batt_status == POWER_SUPPLY_STATUS_DISCHARGING &&
	usb_present){
        // restarting battery charging.
        pval.intval = 0;
        pr_err("[%s], restart charging \n", __func__);
        smblib_set_prop_input_suspend(chip, &pval);
	}*/

	if ((bat_current != chip->nokia_sdm439_last_bat_current) || usb_source_changed_flag) {
		pr_err("[%s] bat_current = %d, current1 = %d, current2 = %d , board_temp = %d, bat_temp = %d, nokia_sdm439_last_bat_current = %d \n",
			__func__, bat_current, bat_current1, bat_current2, board_temp, bat_temp, chip->nokia_sdm439_last_bat_current);

		pr_err("[%s] batt_voltage_now = %d, batt_current_now = %d, input_usb_current_now = %d \n",
			__func__, batt_voltage_now, batt_current_now, usb_current_now);

		pr_err("[%s] batt_status = %d, charge type = %d, usb online = %d \n",
			__func__, batt_status, charge_type, usb_online);

		rc = vote(chip->fcc_votable, BATT_TYPE_FCC_VOTER, true, bat_current);
		if(rc < 0)
			pr_err("[%s] Could not vote charger fcc_votable \n", __func__);

		rc = vote(chip->chg_disable_votable, BATTCHG_USER_EN_VOTER, (bat_current == 0) ? true : false, 0);
		if (rc < 0) {
			dev_err(chip->dev, "[%s] Could not set battchg suspend rc %d\n", __func__, rc);
		}

		chip->nokia_sdm439_last_bat_current = bat_current;
		usb_source_changed_flag = 0;
    }

	rc = smblib_get_charge_param(chip, &chip->param.icl_stat,
				&icl_current);
	if (rc < 0) {
		smblib_err(chip, "Couldn't get ICL status rc=%d\n", rc);
	}
	pr_err("[%s] board_temp = %d, bat_temp = %d, icl_current = %d, chip->batt_profile_fv_uv = %d\n",
		__func__, board_temp, bat_temp, icl_current, chip->batt_profile_fv_uv);

	return rc;
}

/* charging current limitation for projection according to temperature. */
void nokia_sdm439_period_2s_of_thermal_protection(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct smb_charger *chg = container_of(dwork,
                                            struct smb_charger, nokia_sdm439_period_2s_work);

    // If battery charging is enabled
    if (!get_client_vote(chg->chg_disable_votable, USER_VOTER)){
        chg_protection_with_thermal(chg);
    }

	schedule_delayed_work(&chg->nokia_sdm439_period_2s_work,
                          round_jiffies_relative(msecs_to_jiffies(2000)));
}

void nokia_sdm439_smb5_lib_init_vars(void) {
    override_smb5_lib_h_values_for_nokia_sdm439();

	if (nokia_sdm439_mach_get() == NOKIA_SDM439_MACH_DEADPOOL) {
		BATTERY_COLD_TEMP = 1;
		BATTERY_COOL_TEMP = 16;
		BATTERY_WARM_TEMP = 43;
		BATTERY_HOT_TEMP = 48;

		BOARD_GOOD_TEMP = 42;
		BOARD_WARM_TEMP = 46;
		BOARD_HOT_TEMP = 49;

		BATT_COLD_FCC = BATT_CC_0MA;
		BATT_COOL_FCC = BATT_CC_700MA;
		BATT_GOOD_FCC = BATT_CC_2000MA;
		BATT_WARM_FCC = BATT_CC_1200MA;
		BATT_HOT_FCC = BATT_CC_0MA;

		BOARD_GOOD_FCC = BATT_CC_2000MA;
		BOARD_WARM_FCC = BATT_CC_1400MA;
		BOARD_HOT_FCC = BATT_CC_1200MA;

		BATT_WARM_FV = 4100000;
	} else {
		BATTERY_COLD_TEMP = 1;
		BATTERY_COOL_TEMP = 16;
		BATTERY_WARM_TEMP = 45;
		BATTERY_HOT_TEMP = 57;

		BOARD_GOOD_TEMP = 44;
		BOARD_WARM_TEMP = 43;
		BOARD_HOT_TEMP = 60;

		BATT_COLD_FCC = BATT_CC_0MA;
		BATT_COOL_FCC = BATT_CC_900MA;
		BATT_GOOD_FCC = BATT_CC_1200MA;
		BATT_WARM_FCC = BATT_CC_1000MA;
		BATT_HOT_FCC = BATT_CC_0MA;

		BOARD_GOOD_FCC = BATT_CC_1200MA;
		BOARD_WARM_FCC = BATT_CC_1200MA;
		BOARD_HOT_FCC = BATT_CC_1200MA;

		BATT_WARM_FV = 4100000;
	}
}

extern bool nokia_sdm439_isBatteryVaild(void);
void nokia_sdm439_late_smblib_init_hook(struct smb_charger *chg)
{
    int rc = 0;
    if (!nokia_sdm439_isBatteryVaild()) {
        rc = vote(chg->fcc_votable, BATT_TYPE_FCC_VOTER, true, BATT_CC_0MA);
        if(rc < 0)
            pr_err("[%s] Could not vote charger fcc_votable \n", __func__);

        rc = vote(chg->chg_disable_votable, BATTCHG_USER_EN_VOTER, true, 0);
        if (rc < 0)
            dev_err(chg->dev, "[%s] Could not set battchg suspend rc %d\n", __func__, rc);

        pr_err("[%s] No battery valid \n", __func__);
        cancel_delayed_work(&chg->nokia_sdm439_period_2s_work);
    }
}
