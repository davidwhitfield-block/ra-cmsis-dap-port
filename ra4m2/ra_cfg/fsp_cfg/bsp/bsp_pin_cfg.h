/* generated configuration header file - do not edit */
#ifndef BSP_PIN_CFG_H_
#define BSP_PIN_CFG_H_
#include "r_ioport.h"

/* Common macro for FSP header files. There is also a corresponding FSP_FOOTER macro at the end of this file. */
FSP_HEADER

#define PIN_CMSIS_DAP_TDI (BSP_IO_PORT_00_PIN_00)
#define PIN_CMSIS_DAP_TDO (BSP_IO_PORT_00_PIN_01)
#define PIN_CMSIS_DAP_NTRST (BSP_IO_PORT_00_PIN_02)
#define ARDUINO_A4 (BSP_IO_PORT_00_PIN_14)
#define ARDUINO_A5 (BSP_IO_PORT_00_PIN_15)
#define PIN_CMSIS_DAP_SWO (BSP_IO_PORT_01_PIN_00)
#define PIN_CMSIS_DAP_SWDIO (BSP_IO_PORT_01_PIN_01)
#define PIN_CMSIS_DAP_SWCLK (BSP_IO_PORT_01_PIN_02)
#define ARDUINO_D6 (BSP_IO_PORT_01_PIN_03)
#define QSPI_IO2 (BSP_IO_PORT_01_PIN_04)
#define LED1 (BSP_IO_PORT_01_PIN_11)
#define PIN_CMSIS_DAP_RESET (BSP_IO_PORT_01_PIN_12)
#define PMOD1_MISO (BSP_IO_PORT_02_PIN_06)
#define PMOD1_MOSI (BSP_IO_PORT_02_PIN_07)
#define GROVE1_SCL_QWIIC_SCL (BSP_IO_PORT_04_PIN_08)
#define GROVE1_SDA_QWIIC_SDA (BSP_IO_PORT_04_PIN_09)
extern const ioport_cfg_t g_bsp_pin_cfg; /* X2C_AUTOMATION */

void BSP_PinConfigSecurityInit();

/* Common macro for FSP header files. There is also a corresponding FSP_HEADER macro at the top of this file. */
FSP_FOOTER

#endif /* BSP_PIN_CFG_H_ */
