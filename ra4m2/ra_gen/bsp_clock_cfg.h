/* generated configuration header file - do not edit */
#ifndef BSP_CLOCK_CFG_H_
#define BSP_CLOCK_CFG_H_
#define BSP_CFG_CLOCKS_SECURE (0)
#define BSP_CFG_CLOCKS_OVERRIDE (0)
#define BSP_CFG_XTAL_HZ (12000000) /* XTAL 12000000Hz */
#define BSP_CFG_HOCO_FREQUENCY (2) /* HOCO 20MHz */
#define BSP_CFG_PLL_SOURCE (BSP_CLOCKS_SOURCE_CLOCK_MAIN_OSC) /* PLL Src: XTAL */
#define BSP_CFG_PLL_DIV (BSP_CLOCKS_PLL_DIV_1) /* PLL Div /1 */
/* x16.0, not the x10.0 the configurator emitted. The SWD bit-bang is bound by
 * PCLKB, which clocks the I/O ports (R01UH0892EJ table of bus masters/slaves:
 * "PLBIU 50 MHz PCLKB ... I/O ports"), and PCLKB is derived from the PLL, not
 * from ICLK. At x10 the PLL runs at 120 MHz, so PCLKB /4 = 30 MHz and every
 * port store crosses from a 60 MHz CPU onto a half-speed bus - measured ~23
 * CPU cycles per SWD clock, capping SWD at ~2.6 MHz.
 *
 * x16 -> PLL 192 MHz, which keeps every bus inside its documented maximum:
 *   ICLK  /2 = 96 MHz  (max 100)      PCLKA /2 = 96 MHz  (max 100)
 *   PCLKB /4 = 48 MHz  (max 50)       PCLKC /4 = 48 MHz  (max 50)
 *   PCLKD /2 = 96 MHz  (max 100)      FCLK  /4 = 48 MHz  (max 50)
 * PLLMUL supports x10.0..x30.0 in 0.5 steps and PLL2 already runs at 240 MHz
 * for USB, so 192 MHz is comfortably in range.
 *
 * USB is unaffected: UCLK comes from PLL2 (x20 /5 = 48 MHz), not from PLL.
 * FreeRTOS is unaffected: configCPU_CLOCK_HZ is SystemCoreClock, computed at
 * runtime. Keep CPU_CLOCK in src/CMSIS-DAP/DAP_config.h equal to ICLK.
 *
 * Mirrored in configuration.xml as board.clock.pll.mul.160 so regenerating the
 * project from e2 studio does not silently put it back to x10. */
#define BSP_CFG_PLL_MUL BSP_CLOCKS_PLL_MUL(16U,0U) /* PLL Mul x16.0 */
#define BSP_CFG_PLL2_SOURCE (BSP_CLOCKS_SOURCE_CLOCK_MAIN_OSC) /* PLL2 Src: XTAL */
#define BSP_CFG_PLL2_DIV (BSP_CLOCKS_PLL_DIV_1) /* PLL2 Div /1 */
#define BSP_CFG_PLL2_MUL BSP_CLOCKS_PLL_MUL(20U,0U) /* PLL2 Mul x20.0 */
#define BSP_CFG_CLOCK_SOURCE (BSP_CLOCKS_SOURCE_CLOCK_PLL) /* Clock Src: PLL */
#define BSP_CFG_CLKOUT_SOURCE (BSP_CLOCKS_CLOCK_DISABLED) /* CLKOUT Disabled */
#define BSP_CFG_UCK_SOURCE (BSP_CLOCKS_SOURCE_CLOCK_PLL2) /* UCLK Src: PLL2 */
#define BSP_CFG_ICLK_DIV (BSP_CLOCKS_SYS_CLOCK_DIV_2) /* ICLK Div /2 */
#define BSP_CFG_PCLKA_DIV (BSP_CLOCKS_SYS_CLOCK_DIV_2) /* PCLKA Div /2 */
#define BSP_CFG_PCLKB_DIV (BSP_CLOCKS_SYS_CLOCK_DIV_4) /* PCLKB Div /4 */
#define BSP_CFG_PCLKC_DIV (BSP_CLOCKS_SYS_CLOCK_DIV_4) /* PCLKC Div /4 */
#define BSP_CFG_PCLKD_DIV (BSP_CLOCKS_SYS_CLOCK_DIV_2) /* PCLKD Div /2 */
#define BSP_CFG_FCLK_DIV (BSP_CLOCKS_SYS_CLOCK_DIV_4) /* FCLK Div /4 */
#define BSP_CFG_CLKOUT_DIV (BSP_CLOCKS_SYS_CLOCK_DIV_1) /* CLKOUT Div /1 */
#define BSP_CFG_UCK_DIV (BSP_CLOCKS_USB_CLOCK_DIV_5) /* UCLK Div /5 */
#endif /* BSP_CLOCK_CFG_H_ */
