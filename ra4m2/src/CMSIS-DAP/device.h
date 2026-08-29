/*---------------------------------------------------------------------------
 * device.h - port-local shim. NOT part of the ARM CMSIS Pack.
 *
 * DAP_config.h opens with:
 *
 *     #ifdef _RTE_
 *     #include "RTE_Components.h"
 *     #include CMSIS_device_header
 *     #else
 *     #include "device.h"
 *     #endif
 *
 * _RTE_ is a Keil MDK / CMSIS-Pack build symbol and is never defined here, so the
 * #else branch is the one taken and this file has to supply everything CMSIS-DAP
 * expects of a device header: the CMSIS core types, __STATIC_INLINE /
 * __STATIC_FORCEINLINE, and the DWT and SCB definitions that DAP_config.h's
 * TIMESTAMP_GET() and DAP.c use.
 *
 * FSP's bsp_api.h pulls in all of that transitively (renesas.h -> the CMSIS core
 * headers), so one include is the whole file. It looks like a stray - it carries
 * no ARM Apache banner, unlike every other header in this directory - but
 * deleting it breaks the build in a way that reads as a missing CMSIS pack.
 * osObjects.h next to it is the same kind of shim.
 *
 * The commented-out lines below are the original author's absolute paths into a
 * checkout at /Users/Patri/OneDrive/... - a machine nobody here has. They are
 * kept only as a record of which FSP headers this shim is standing in for
 * (bsp_feature.h, system.h, renesas.h); bsp_api.h reaches all three. Do not
 * uncomment them.
 *---------------------------------------------------------------------------*/

//#include <stdint.h>
//#define __STATIC_INLINE
//#define __STATIC_FORCEINLINE
//#include ".../ra/fsp/src/bsp/mcu/ra4m2/bsp_feature.h"
//#include ".../ra/fsp/src/bsp/cmsis/Device/RENESAS/Include/system.h"
//#include ".../ra/fsp/src/bsp/cmsis/Device/RENESAS/Include/renesas.h"
#include "bsp_api.h"
