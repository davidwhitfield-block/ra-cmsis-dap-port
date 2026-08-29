/***********************************************************************************************************************
 * File Name    : common_utils.h
 * Description  : Contains macros, data structures and functions used  common to the EP
 ***********************************************************************************************************************/
/***********************************************************************************************************************
 * DISCLAIMER
 * This software is supplied by Renesas Electronics Corporation and is only intended for use with Renesas products. No
 * other uses are authorized. This software is owned by Renesas Electronics Corporation and is protected under all
 * applicable laws, including copyright laws.
 * THIS SOFTWARE IS PROVIDED "AS IS" AND RENESAS MAKES NO WARRANTIES REGARDING
 * THIS SOFTWARE, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. ALL SUCH WARRANTIES ARE EXPRESSLY DISCLAIMED. TO THE MAXIMUM
 * EXTENT PERMITTED NOT PROHIBITED BY LAW, NEITHER RENESAS ELECTRONICS CORPORATION NOR ANY OF ITS AFFILIATED COMPANIES
 * SHALL BE LIABLE FOR ANY DIRECT, INDIRECT, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES FOR ANY REASON RELATED TO THIS
 * SOFTWARE, EVEN IF RENESAS OR ITS AFFILIATES HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 * Renesas reserves the right, without notice, to make changes to this software and to discontinue the availability of
 * this software. By using this software, you agree to the additional terms and conditions found by accessing the
 * following link:
 * http://www.renesas.com/disclaimer
 *
 * Copyright (C) 2020 Renesas Electronics Corporation. All rights reserved.
 ***********************************************************************************************************************/

/* Logging over SEGGER RTT, plus a few shared constants.
 *
 * IMPORTANT: every APP_PRINT / APP_ERR_PRINT below writes into an RTT control
 * block in the *probe's* RAM. It is invisible unless a second debugger is
 * attached to this board and running an RTT client - i.e. the J-Link on the
 * bench, not the target being debugged. Nothing appears on the VCOM port. So a
 * fault path whose only symptom is an APP_ERR_PRINT is, in normal standalone
 * operation, entirely silent; the status LED is the only feedback the user gets.
 *
 * RTT output is also not free. SEGGER_RTT_printf() formats and memcpy()s under a
 * lock, from whatever context called it - including the DAP command loop. Do not
 * add printing to a hot path.
 */
#ifndef COMMON_UTILS_H_
#define COMMON_UTILS_H_

/* generic headers */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "hal_data.h"
/* SEGGER RTT and error related headers */
#include "SEGGER_RTT/SEGGER_RTT.h"


#define BIT_SHIFT_8  (8u)
#define SIZE_64      (64u)

#define LVL_ERR      (1u)       /* error conditions   */
#define LVL_DEBUG    (3u)       /* debug-level messages */

#define LOG_LEVEL    LVL_ERR    /* To See the Debug Messages, LOG_LEVEL should be set to LVL_DEBUG */

#define RESET_VALUE             (0x00)

#define SEGGER_INDEX            (0)

#define APP_PRINT(fn_, ...)      SEGGER_RTT_printf (SEGGER_INDEX,(fn_), ##__VA_ARGS__);

#define APP_ERR_PRINT(fn_, ...)  if(LVL_ERR)\
        SEGGER_RTT_printf (SEGGER_INDEX, "[ERR] In Function: %s(), %s",__FUNCTION__,(fn_),##__VA_ARGS__);

/* Halts the probe. BKPT #0 with no debugger attached does not break - it escalates
 * to a HardFault, and the firmware is dead: USB stops answering, the LED freezes
 * at whatever level it last held, and only a power cycle recovers it. That is the
 * normal standalone case, since the RTT message above it goes nowhere either.
 *
 * handle_error() in dap_thread_entry.c reaches this on several ordinary FSP error
 * returns (R_USB_Open, R_SCI_UART_Open/Write, R_USB_Write), so a transient failure
 * on the VCOM path can take the whole probe down. Deliberately left as-is: this is
 * FSP example-code convention, and changing it is a behavioural change. If you see
 * a board that enumerated once and then went completely unresponsive with a frozen
 * LED, suspect this before suspecting USB. */
#define APP_ERR_TRAP(err)        if(err) {\
        SEGGER_RTT_printf(SEGGER_INDEX, "\r\nReturned Error Code: 0x%x  \r\n", err);\
        __asm("BKPT #0\n");} /* trap upon the error  */

#define APP_READ(read_data)     SEGGER_RTT_Read (SEGGER_INDEX, read_data, sizeof(read_data));

#define APP_CHECK_DATA          SEGGER_RTT_HasKey()

#define APP_DBG_PRINT(fn_, ...) if(LOG_LEVEL >= LVL_DEBUG)\
        SEGGER_RTT_printf (SEGGER_INDEX, "[DBG] In Function: %s(), %s",__FUNCTION__,(fn_),##__VA_ARGS__);


#endif /* COMMON_UTILS_H_ */
