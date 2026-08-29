/***********************************************************************************************************************
 * File Name    : usb_composite.h
 * Description  : Contains macros, data structures and function declaration used in EP
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
/* Shared definitions for the composite USB device: the VCOM (CDC) side, the
 * Microsoft OS descriptor structures the CMSIS-DAP interface needs on Windows, and
 * the SWO plumbing types. Included by dap_thread_entry.c, swo_thread_entry.c and
 * r_usb_pcdc_pvnd_descriptor.c. */
#ifndef USB_COMPOSITE_H_
#define USB_COMPOSITE_H_

#define LINE_CODING_LENGTH (0x07U) // Line coding length
/* Must equal the CDC bulk endpoint wMaxPacketSize in r_usb_pcdc_pvnd_descriptor.c
 * (USB_MXPS_BULK_FULL). It sizes both PCDC_ToTarget[][] and the g_PCDC_tx_data
 * staging buffer in dap_thread_entry.c. */
#define CDC_DATA_LEN (64U)
/* Depth of the VCOM host->target ring. 255 x 64 = ~16 KB of .bss; unlike
 * DAP_PACKET_COUNT this was never tuned, and it is generous for a serial bridge.
 * Wrapping is done by explicit compare, not by masking, so it need not be 2^n. */
#define UART_PACKET_COUNT 255

/* Macro definitions */
/* FSP's sentinel for "no flow-control pin configured", checked before touching
 * g_uart_ctrl.flow_pin on a SET_CONTROL_LINE_STATE. */
#define SCI_UART_INVALID_16BIT_PARAM   (0xFFFFU)
/* Max baud error accepted by R_SCI_UART_BaudCalculate(), in 1/1000 percent -
 * 5000 = 5%. Applies to both the VCOM line coding and the SWO baud request. */
#define BAUD_ERROR_RATE                (5000U)
#define INVALID_SIZE                   (0U)
/* Interface numbers, and they must agree with the configuration descriptor in
 * r_usb_pcdc_pvnd_descriptor.c and with the two sections of `ecd` (the Extended
 * Compat ID descriptor). Interfaces 0 and 1 are the CDC pair; 2 is CMSIS-DAP. The
 * dap_thread_entry.c handler for USB_VENDOR_GET_MS_DESCRIPTOR_INTERFACE compares
 * the request's low byte against INTERFACE_CMSIS_DAP, so a mismatch here makes
 * Windows fail to bind WinUSB with no other symptom. */
#define INTERFACE_PCDC_FIRST 0x0
#define INTERFACE_CMSIS_DAP 0x2

// Extended Compat ID Descriptor Format
typedef __PACKED_STRUCT 
{
    uint8_t bFirstInterfaceNumber; // The interface or function number
    uint8_t RESERVED1[1];          // Reserved for system use. Set this value to 0x01
    uint8_t compatibleID[8];       // The function’s compatible ID
    uint8_t subCompatibleID[8];    // The function’s subcompatible ID
    uint8_t RESERVED2[6];          // Reserved
} tyECIDDescFunctionSection;


typedef __PACKED_STRUCT 
{
    uint32_t dwLength;      // The length, in bytes, of the complete extended compat ID descriptor
    uint16_t bcdVersion;    // The descriptor’s version number, in binary coded decimal (BCD) format
    uint16_t wIndex;        // An index that identifies the particular OS feature descriptor
    uint8_t  bCount;        // The number of custom property sections
    uint8_t  RESERVED[7];  // Reserved . Fill this value with NULLs.

} tyExtendedCompatIDDescriptor;

typedef __PACKED_STRUCT 
{
    tyExtendedCompatIDDescriptor hdr;
    tyECIDDescFunctionSection    pvnd;
    tyECIDDescFunctionSection    pcdc;
} tyRAM4ECID;


typedef struct _SWO_UART_STAT {
  uint32_t HW_FIFO_OVERFLOW;      ///< Number of UART_EVENT_ERR_OVERFLOW events
  uint32_t QUEUE_OVERFLOW;        ///< Number of bytes that could not be pushed to queue
  uint32_t SMALLEST_RCV;     ///< Smallest request to receive on UART
  uint32_t LARGEST_RCV;      ///< Largest request to receive on UART
} SWO_UART_STAT;

typedef struct _SWO_USB_STAT {
  uint32_t SMALLEST_TX;     ///< Smallest request to transmit to host
  uint32_t LARGEST_TX;      ///< Largest request to transmit to host
} SWO_USB_STAT;
void ProcessUartSwoQueue(void);
void ProcessUsbSwoQueue(void);
/* One SWO->USB transfer request, posted to g_queue_swo_usb by
 * swo_thread_entry.c's SWO_QueueTransfer() and issued by ProcessUsbSwoQueue() in
 * dap_thread_entry.c. {buf = NULL, num = 0} is the agreed abort sentinel, meaning
 * "stop the pipe" rather than "write nothing". */
typedef __PACKED_STRUCT {
    uint8_t *buf;
    uint32_t num;
} SWO_USB_REQUEST;

/* bRequest for the Microsoft OS 1.0 vendor requests. Windows takes this byte from
 * the MSFT100 string descriptor at string index 0xEE and echoes it back as
 * bRequest, so this constant must equal the one embedded in that descriptor in
 * r_usb_pcdc_pvnd_descriptor.c.
 *
 * 0x00 is an unusual choice - most devices use something conspicuous like 0x5E -
 * but it is legal, and it works because the two macros below fold it together with
 * the direction/type/recipient bits, giving request_type values that cannot
 * collide with the standard or CDC class requests handled alongside them. */
#define MS_VENDOR_CODE_CMSIS_DAP 0x00

// Extended compat ID OS descriptor.
#define EXT_COMPATID_OS_DESCRIPTOR 0x04
// Extended Properties OS Descriptor
#define EXT_PROP_OS_DESCRIPTOR 0x05
#define USB_VENDOR_GET_MS_DESCRIPTOR_DEVICE      ((MS_VENDOR_CODE_CMSIS_DAP << 8) | USB_DEV_TO_HOST | USB_VENDOR | USB_DEVICE)
#define USB_VENDOR_GET_MS_DESCRIPTOR_INTERFACE   ((MS_VENDOR_CODE_CMSIS_DAP << 8) | USB_DEV_TO_HOST | USB_VENDOR | USB_INTERFACE)


#endif /* USB_COMPOSITE_H_ */
