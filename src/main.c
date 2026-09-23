/*==================================================================================================
* Project : RTD AUTOSAR 4.9
* Platform : CORTEXM
* Peripheral : S32K3XX
* Dependencies : none
*
* Autosar Version : 4.9.0
* Autosar Revision : ASR_REL_4_9_REV_0000
* Autosar Conf.Variant :
* SW Version : 7.0.1
* Build Version : S32K3_RTD_7_0_1_D2602_ASR_REL_4_9_REV_0000_20260206
*
* Copyright 2020 - 2026 NXP
*
*   NXP Proprietary. This software is owned or controlled by NXP and may only be
*   used strictly in accordance with the applicable license terms. By expressly
*   accepting such terms or by downloading, installing, activating and/or otherwise
*   using the software, you are agreeing that you have read, and that you agree to
*   comply with and are bound by, such license terms. If you do not agree to be
*   bound by the applicable license terms, then you may not retain, install,
*   activate or otherwise use the software.
==================================================================================================*/

/**
*   @file main.c
*
*   @addtogroup main_module main module documentation
*   @{
*/

/* User includes */
#include "Pwm.h"
#include "Mcl.h"
#include "Mcu.h"
#include "Siul2_Port_Ip.h"
#include "Icu.h"
#include "CDD_Uart.h"
#include "Port.h"
#include "Platform.h"
#include "Adc.h"
#include "freemaster.h"
#include "freemaster_s32_lpuart.h"


/* -------------------------------------------------------------------------
 * Board push-button configuration (SW2 = increase, SW3 = decrease step).
 * The buttons are read by the AUTOSAR Icu driver using interrupts (edge
 * detection) instead of polling the Dio driver:
 *   - SW3 (PTB19) -> IcuWkpu  ch38   -> logical IcuChannel_0 -> Icu_SW3_Notification
 *   - SW2 (PTB26) -> IcuSiul2 EIRQ13 -> logical IcuChannel_1 -> Icu_SW2_Notification
 * Both channels are configured for ICU_MODE_SIGNAL_EDGE_DETECT / ICU_FALLING_EDGE
 * (buttons are active-low, so a press is a falling edge). The step change is
 * done directly in the notification callbacks below.
 * The interrupt vectors are installed by Platform_Init via the generated
 * interrupt controller config (WKPU_EXT_IRQ_SINGLE_ISR on WKPU_IRQn and
 * SIUL2_EXT_IRQ_8_15_ISR on SIUL_1_IRQn for the EIRQ8-15 group).
 *
 * BTN_STEP_INCREMENT is how much one button press changes breathe_speed, with
 * the clamp limits. Matches the FreeMASTER slider range/step for breathe_speed. */
#define BTN_STEP_INCREMENT ((sint32)1)
#define BTN_STEP_MIN       ((sint32)1)
#define BTN_STEP_MAX       ((sint32)20)

/* Global variables for FreeMASTER */
float32         temperature;
bool            error_flag;
uint16 			dutyCycle = 0U;

/* Breathing (triangular fade) animation parameters.
 * These are global variables (not #defines) so they can be watched and changed
 * live from the FreeMASTER PC application while the target is running.
 * Notes:
 *  - breathe_speed is the brightness change applied on every loop pass.
 *    Smaller value = smoother, slower ramp; larger = faster, coarser ramp.
 *  - breathe_poll_delay is a busy-wait spin count that paces the loop. Keep it
 *    at or below 500 to ensure FreeMASTER stays within the response window. */
volatile sint32 breathe_duty_max   = (sint32)0x8000;    /* peak brightness (<= 0x8000) */
volatile sint32 breathe_speed      = (sint32)3;         /* brightness change per tick (1-20 range) */
volatile uint32 breathe_poll_delay = (uint32)50U;       /* busy-wait pacing, max 500   */


void TestDelay(uint32 delay);

/* Required ADC group-end notification callback */
void Adc_Adc0Group0Notification(void)
{
	/* Polling mode (not used in this demo) */
}

void TestDelay(uint32 delay)
{
    static volatile uint32 DelayTimer = 0;
    while(DelayTimer<delay)
    {
        DelayTimer++;
    }
    DelayTimer=0;
}

/**
 * @brief SW2 (PTB26, IcuSiul2 EIRQ13 -> IcuChannel_1) falling-edge notification.
 *        Increases breathe_speed, clamped to BTN_STEP_MAX.
 *        Runs in ICU interrupt context, keep it short.
 */
void Icu_SW2_Notification(void)
{
    breathe_speed += BTN_STEP_INCREMENT;
    if (breathe_speed > BTN_STEP_MAX)
    {
        breathe_speed = BTN_STEP_MAX;
    }
}

/**
 * @brief SW3 (PTB19, IcuWkpu ch38 -> IcuChannel_0) falling-edge notification.
 *        Decreases breathe_speed, clamped to BTN_STEP_MIN.
 *        Runs in ICU interrupt context, keep it short.
 */
void Icu_SW3_Notification(void)
{
    breathe_speed -= BTN_STEP_INCREMENT;
    if (breathe_speed < BTN_STEP_MIN)
    {
        breathe_speed = BTN_STEP_MIN;
    }
}


int main(void)
{
    Adc_CalibrationStatusType  calStatus;
    uint16                     adcResult;
    Std_ReturnType             adcStatus;
    sint32                     duty = 0;             /* current LED brightness   */
    sint32                     step = breathe_speed;  /* per-tick increment       */

    /* Initialize OsIf */
    OsIf_Init(NULL_PTR);

    /* Initialize Mcu driver */
    Mcu_Init(&Mcu_Config);
    /* Initialize clock */
    Mcu_InitClock(McuClockSettingConfig_0);

#if (MCU_NO_PLL == STD_OFF)
    while (MCU_PLL_LOCKED != Mcu_GetPllStatus())
    {
        /* Wait for PLL lock */
    }
    /* Activate PLL clock */
    Mcu_DistributePllClock();
#endif
    /* Activate MCU power mode */
    Mcu_SetMode(McuModeSettingConf_0);

    /* Initialize Mcl driver */
    Mcl_Init(&Mcl_Config);

    /* Initialize all pins using the Siul2_Port driver */
    Siul2_Port_Ip_Init(NUM_OF_CONFIGURED_PINS, g_pin_mux_InitConfigArr);
    Port_Init(NULL_PTR);

    /* Activate Interrupts */
    Platform_Init(NULL_PTR);
    /* Initialize the Icu driver and enable interrupt-based edge detection and
     * notifications for the two board buttons:
     *   IcuChannel_0 -> SW3 (PTB19, WKPU ch38) -> Icu_SW3_Notification
     *   IcuChannel_1 -> SW2 (PTB26, EIRQ 13)   -> Icu_SW2_Notification */
    Icu_Init(&Icu_Config);
    Icu_EnableEdgeDetection(IcuConf_IcuChannel_IcuChannel_0);
    Icu_EnableEdgeDetection(IcuConf_IcuChannel_IcuChannel_1);
    Icu_EnableNotification(IcuConf_IcuChannel_IcuChannel_0);
    Icu_EnableNotification(IcuConf_IcuChannel_IcuChannel_1);

    /* Initialize Pwm driver */
    Pwm_Init(&Pwm_Config);

    Uart_Init(NULL_PTR);
    /* Set FreeMASTER serial base address. */
	FMSTR_SerialSetBaseAddress((FMSTR_ADDR)IP_LPUART_6_BASE);
    /* FreeMASTER Initialization */
    FMSTR_Init();

    /* ADC, TempSense init */
    Adc_Init(NULL_PTR);
    Adc_Calibrate(0U, &calStatus);
    (void)calStatus;
    Adc_TempSenseSetPowerMode(ADC_NORMAL_MODE);

    /* Adc_TempSenseGetTemp() triggers an internal ADC conversion on the TempSense
     * channel, waits for completion, and writes the result to adcResult in Q11.4
     * fixed-point format (1 sign bit, 11 integer bits, 4 fractional bits).
     * Dividing by 16 converts the Q11.4 value to degrees Celsius.             */
    while (1)
    {
        adcStatus = Adc_TempSenseGetTemp(0U, &adcResult);
        if (E_OK == adcStatus)
        {
            temperature  = (float32)((sint16)adcResult) / 16.0f;
            error_flag   = FALSE;
        }
        else
        {
            error_flag = TRUE;
        }
        /* Service FreeMASTER on every loop pass so the response never times out.
         * The pacing delay here is tiny, so FMSTR_Poll() is called very often and
         * always stays inside the response window. */
        FMSTR_Poll();
        /* Advance the breathing animation on every loop pass.
         * breathe_speed controls how big each brightness step is (smaller = smoother).
         * breathe_poll_delay controls loop pacing.
         * All parameters are read from the global variables so changes made in
         * FreeMASTER take effect live. */
        TestDelay(breathe_poll_delay);

        /* Board buttons are handled asynchronously by the Icu interrupt
         * notifications (Icu_SW2_Notification / Icu_SW3_Notification), so no
         * polling is needed here. */

        /* Re-apply magnitude from breathe_speed while preserving fade direction. */
        step = (step < 0) ? -breathe_speed : breathe_speed;

        duty += step;
        if (duty >= breathe_duty_max)
        {
            duty = breathe_duty_max;
            step = -step;             /* reached full brightness -> fade out */
        }
        else if (duty <= 0)
        {
            duty = 0;
            step = -step;             /* reached zero -> fade in */
        }

        Pwm_SetDutyCycle(EMIOS_1_CH_12, (uint16)duty);
        dutyCycle = (uint16)duty;     /* mirror to FreeMASTER variable */
    }
    return 0;
}
