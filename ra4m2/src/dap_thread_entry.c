/***********************************************************************************************************************
 * dap_thread_entry.c - the probe's main thread.
 *
 * Everything the host asks of this board arrives here. One FreeRTOS task owns
 * three logical streams, all multiplexed onto one composite USB device:
 *
 *   CMSIS-DAP v2   bulk OUT `cmd_pipe` -> DAP_ExecuteCommand() -> bulk IN `rsp_pipe`.
 *                  This is the debug probe proper. Pipe numbers are discovered at
 *                  SET_CONFIGURATION, not hardcoded - see process_usb_events().
 *   VCOM (CDC)     host bulk OUT -> SCI2 TXD (P302), SCI2 RXD (P301) -> host bulk IN.
 *                  A plain USB-serial bridge to the target, unrelated to SWD.
 *   SWO            SCI0 RXD (P100) -> ../src/swo_thread_entry.c -> bulk IN `swo_pipe`.
 *                  Only the USB half lives here (ProcessUsbSwoQueue()).
 *
 * Structure, in the order things happen:
 *   dap_thread_entry()    one-time setup (serial number from the device UID, USB
 *                         open, UART open, semaphores), then an endless loop that
 *                         drains the USB event queue, runs DAP commands, services
 *                         VCOM in both directions, pumps SWO, and updates the LED.
 *   process_usb_events()  the state machine behind that queue: enumeration, read
 *                         and write completions, CDC class requests, suspend/detach.
 *   usb_composite_callback()  FSP's callback; posts to the queue and wakes the thread.
 *   user_uart_callback()  SCI2 ISR side of VCOM.
 *   status_led_update()   drives DS11.
 *
 * The thread never blocks for long: it parks on xSemaphoreTake(g_sem_DAP_Thread, 1),
 * so it wakes on any USB event and otherwise turns over once per tick (1 ms). That
 * 1-tick timeout is also what guarantees the LED keeps moving on a quiet bus.
 *
 * Concurrency: the USB_* and PCDC_* ring variables below are touched from this
 * thread and from process_usb_events(), which runs on this same thread (it is
 * called from the loop, not from the callback) - so they need no locking against
 * each other. user_uart_callback() is the only genuine ISR context, and it talks
 * only through FreeRTOS queues and semaphores.
 **********************************************************************************************************************/

#include "dap_thread.h"
#include "common_utils.h"
#include "usb_composite.h"
#include "CMSIS-DAP/DAP_config.h"
#include "CMSIS-DAP/DAP.h"
#include "r_usb_pcdc_cfg.h"
#include "CMSIS-DAP/Driver_USART.h"

/**********************************************************************************************************************
 * @addtogroup usb_composite_ep
 * @{
 **********************************************************************************************************************/

#define UART_TXING 0x1
#define UART_RXING 0x2

/* Status LED (DS11 on P111, JLINK_OB_LED_L, active low).
 *
 * Behaves like a J-Link: solid while idle and enumerated, pulsing while debug
 * traffic is actually moving over SWD, slow blink if the board never enumerated.
 * The pulse is what makes it obvious at a glance whether a read, an erase or a
 * flash is really running, rather than the host being wedged.
 *
 * The indication is a *blank*, not a square wave: the LED rests on and is driven
 * off briefly, because a symmetric toggle fast enough to track a bulk transfer
 * lands around 12 Hz and the eye integrates that into a slightly dim steady glow
 * - indistinguishable from idle.
 *
 * The blank rate is proportional to DATA VOLUME, one blank per
 * ACTIVITY_LED_BYTES moved, so the LED tells you how much is flowing and not
 * merely that something is: ~3 Hz at half the ceiling rate, ~1 Hz for a trickle.
 * Bounded at both ends - MIN_PERIOD keeps a saturated transfer from blurring back
 * into a dim glow, MAX_PERIOD keeps a very slow one (single register pokes from a
 * debugger) visibly alive.
 *
 * Mind the arithmetic on the two bounds: both name the ON phase only, and every
 * cycle costs an extra ACTIVITY_LED_OFF_TICKS of blank on top. So the real period
 * is MIN_PERIOD + OFF_TICKS = 195 ms (~5.1 Hz) at the fast end and MAX_PERIOD +
 * OFF_TICKS = 1245 ms (~0.80 Hz) at the slow end. 48 KiB per blank at the measured
 * ~313 KiB/s SWD ceiling wants ~6.5 Hz, so a saturated transfer sits pinned at the
 * MIN_PERIOD cap and the flicker stops getting faster - which is the point of the
 * cap, but it also means "as fast as it goes" is ~5 Hz, not 6.7. */
#define STATUS_LED_BLINK_TICKS  pdMS_TO_TICKS(500)      /* not enumerated: half period */
#define ACTIVITY_LED_BYTES      (48U * 1024U)           /* data moved per blank */
#define ACTIVITY_LED_OFF_TICKS  pdMS_TO_TICKS(45)       /* blank width - long enough to see */
#define ACTIVITY_LED_MIN_PERIOD pdMS_TO_TICKS(150)      /* shortest ON phase; +45 ms = ~5.1 Hz */
#define ACTIVITY_LED_MAX_PERIOD pdMS_TO_TICKS(1200)     /* longest ON phase;  +45 ms = ~0.80 Hz */
#define ACTIVITY_LED_IDLE_TICKS pdMS_TO_TICKS(300)      /* quiet this long -> back to solid */

/* Bytes moved over SWD, accumulated per executed DAP command as the larger of
 * the command and its response - a block read is 5 bytes in and ~1 KiB out, a
 * block write the reverse, and it is the big side that was on the wire. Written
 * and read only by the DAP thread, so it needs no synchronisation; volatile just
 * keeps the compiler from caching it across the loop. Free to wrap: every
 * comparison is an unsigned difference. */
static volatile uint32_t g_dap_bytes;

/* external variables
 *
 * All but g_bsp_leds are defined in src/r_usb_pcdc_pvnd_descriptor.c: the USB
 * descriptor set, the serial-number string that dap_thread_entry() rewrites from
 * the device UID, and the two Microsoft OS descriptors (`ecd` = extended compat ID,
 * ExtendedPropertiesDescriptor = the WinUSB GUID) that let Windows bind WinUSB to
 * the CMSIS-DAP interface without an .inf.
 *
 * g_bsp_leds comes from FSP's ra_gen/bsp_pin_cfg or the BSP board file and is
 * declared `const bsp_leds_t` there. Re-declaring it non-const here is a type
 * mismatch that the linker does not diagnose; it works because status_led_update()
 * only ever reads it. Left as-is rather than corrected, because changing it is a
 * code change needing a rebuild and reflash to validate - see the audit notes.
 *
 * SWO_TransferComplete() is prototyped by hand instead of via a header because
 * CMSIS-DAP/SWO.c does not export one. */
extern uint8_t g_apl_configuration[];
extern uint8_t g_apl_report[];
extern bsp_leds_t g_bsp_leds;
extern uint8_t g_apl_string_descriptor_serial_number[];
extern uint8_t ExtendedPropertiesDescriptor[];
extern tyRAM4ECID ecd;
void SWO_TransferComplete(void);

/* Local Module Variables */

/* True from SET_CONFIGURATION until the device is physically detached.
 *
 * This is the single gate on almost everything: it selects solid-vs-slow-blink on
 * the LED, and it guards the USB_STATUS_READ_COMPLETE and USB_STATUS_WRITE_COMPLETE
 * bodies in process_usb_events(), so while it is false every completed transfer is
 * dropped on the floor.
 *
 * Set in exactly one place (USB_STATUS_CONFIGURED, after the pipes are discovered
 * and the first reads are posted) and cleared in exactly one place
 * (USB_STATUS_DETACH). Notably NOT cleared on USB_STATUS_SUSPEND - see the long
 * comment on that case for why that used to be a one-way door.
 *
 * scripts/flash.sh reads this symbol's address out of the ELF with
 * arm-none-eabi-nm and reports its value after a flash, so it is deliberately a
 * file-scope static with a stable name rather than a local or a bitfield. */
static bool b_usb_configured = false;

/*******************************************************************************************************************//**
 * @brief Drive the board status LED from USB enumeration state and SWD activity.
 *
 * Three states:
 *   not enumerated      slow blink, so a board that never came up is obvious
 *   enumerated, idle    solid on
 *   enumerated, busy    pulsing, at a rate that tracks the DAP command rate
 *
 * Called from the DAP thread loop, which turns over at least once per tick.
 * The pin is only written when the level actually changes, so a loop spinning
 * at full rate does not put a redundant port write between every DAP command.
 **********************************************************************************************************************/
static void status_led_update (void)
{
    if (LED_INDEX_STATUS >= g_bsp_leds.led_count)
    {
        return;                        /* board has no status LED */
    }

    static bsp_io_level_t last_level    = LED_STATUS_OFF_LEVEL;
    static bool           level_valid   = false;
    static uint32_t       seen_bytes    = 0U;   /* byte count at the last blank */
    static uint32_t       prev_bytes    = 0U;   /* for idle detection */
    static TickType_t     phase_start   = 0U;   /* when the current on/off phase began */
    static TickType_t     last_activity = 0U;
    static bool           pulse_off     = false;

    TickType_t     now   = xTaskGetTickCount();
    uint32_t       bytes = g_dap_bytes;
    bsp_io_level_t level;

    if (bytes != prev_bytes)
    {
        prev_bytes    = bytes;
        last_activity = now;
    }

    if (!b_usb_configured)
    {
        bool on = ((now / STATUS_LED_BLINK_TICKS) & 1U) != 0U;
        level = on ? LED_STATUS_ON_LEVEL : LED_STATUS_OFF_LEVEL;
    }
    else if ((TickType_t) (now - last_activity) < ACTIVITY_LED_IDLE_TICKS)
    {
        TickType_t since = (TickType_t) (now - phase_start);

        if (pulse_off)
        {
            /* End the blank once it has been off long enough to register. */
            if (since >= ACTIVITY_LED_OFF_TICKS)
            {
                pulse_off   = false;
                phase_start = now;
            }
        }
        else if ((since >= ACTIVITY_LED_MIN_PERIOD) &&
                 (((uint32_t) (bytes - seen_bytes) >= ACTIVITY_LED_BYTES) ||
                  (since >= ACTIVITY_LED_MAX_PERIOD)))
        {
            /* Blank once another ACTIVITY_LED_BYTES have gone over the wire, so
             * the cadence is throughput. The MAX_PERIOD arm is the floor: a host
             * dribbling single-word transfers still blinks, just slowly. */
            seen_bytes  = bytes;
            pulse_off   = true;
            phase_start = now;
        }

        level = pulse_off ? LED_STATUS_OFF_LEVEL : LED_STATUS_ON_LEVEL;
    }
    else
    {
        seen_bytes  = bytes;
        pulse_off   = false;
        phase_start = now;
        level       = LED_STATUS_ON_LEVEL;
    }

    if (!level_valid || (level != last_level))
    {
        R_BSP_PinWrite((bsp_io_port_pin_t) g_bsp_leds.p_leds[LED_INDEX_STATUS], level);
        last_level  = level;
        level_valid = true;
    }
}
static usb_pcdc_linecoding_t g_line_coding;

/* The two CMSIS-DAP rings, request (host -> probe) and response (probe -> host).
 *
 * The naming is upstream ARM's and is easy to read backwards. "I" is IN to the
 * ring (producer) and "O" is OUT of it (consumer) - NOT the USB sense of IN/OUT.
 * So for the request ring the producer is USB (a completed bulk OUT read bumps
 * IndexI/CountI) and the consumer is the DAP loop; for the response ring the
 * producer is the DAP loop and the consumer is USB.
 *
 * Each ring carries both an index and a count. The index wraps at
 * DAP_PACKET_COUNT and says *where*; the free-running 16-bit count says *how many*
 * and is what the fullness tests use, because IndexI == IndexO is ambiguous
 * between empty and full. Every such test is written as an unsigned difference
 * ((uint16_t)(CountI - CountO)), so wrap at 65535 is harmless. There is no
 * modular arithmetic on the indices - they are stepped and compared against
 * DAP_PACKET_COUNT by hand - so DAP_PACKET_COUNT does *not* have to be a power of
 * two (unlike the SWO capture ring in swo_thread_entry.c, which does).
 *
 * The Idle flags close the loop between the two halves. `USB_RequestIdle` means
 * "no bulk OUT read is posted, because the ring was full"; whoever frees a slot
 * re-posts the read. `USB_ResponseIdle` means "no bulk IN write is in flight";
 * whoever produces a response starts one. Miss either hand-off and the pipeline
 * stalls silently with the host waiting forever.
 *
 * `volatile` here is habit rather than necessity: every one of these is touched
 * only from the DAP thread (process_usb_events() is called from the loop, not from
 * the USB callback), so there is no cross-context access to order.
 *
 * Sizing: DAP_PACKET_COUNT x DAP_PACKET_SIZE is 8 x 1024, so these two arrays are
 * 16 KB of .bss on a 128 KB part. That is the reason DAP_PACKET_COUNT is 8 and not
 * upstream's 255 - see CMSIS-DAP/DAP_config.h. */
static volatile uint16_t USB_RequestIndexI; // Request Index In
static volatile uint16_t USB_RequestIndexO; // Request Index Out
static volatile uint16_t USB_RequestCountI; // Request Count In
static volatile uint16_t USB_RequestCountO; // Request Count Out
static volatile uint8_t USB_RequestIdle;    // Request Idle  Flag

static volatile uint16_t USB_ResponseIndexI; // Response Index In
static volatile uint16_t USB_ResponseIndexO; // Response Index Out
static volatile uint16_t USB_ResponseCountI; // Response Count In
static volatile uint16_t USB_ResponseCountO; // Response Count Out
static volatile uint8_t USB_ResponseIdle;    // Response Idle  Flag

static uint8_t USB_Request[DAP_PACKET_COUNT][DAP_PACKET_SIZE];
static uint8_t USB_Response[DAP_PACKET_COUNT][DAP_PACKET_SIZE];
/* Responses are variable length, so unlike requests the ring needs to carry the
 * length alongside the buffer - it is what R_USB_PipeWrite() is given. */
static uint16_t USB_RespSize[DAP_PACKET_COUNT];

/* The VCOM host->target ring. Same discipline as the DAP request ring above and
 * the same I/O naming, just a different depth (UART_PACKET_COUNT) and width
 * (CDC_DATA_LEN, the CDC bulk endpoint size).
 *
 * The trailing "Request ..." comments on the next five lines are copy-paste from
 * the DAP block and mean nothing here - this ring carries VCOM bytes destined for
 * the target's UART, not DAP requests. Kept only so a diff against upstream stays
 * legible.
 *
 * There is no matching ToHost ring: the target->host direction goes straight from
 * user_uart_callback() into g_queue_uart_tx_8 / _16 and is drained into
 * g_PCDC_tx_data in the main loop. */
static volatile uint16_t PCDC_ToTargetIndexI; // Request Index In
static volatile uint16_t PCDC_ToTargetIndexO; // Request Index Out
static volatile uint16_t PCDC_ToTargetCountI; // Request Count In
static volatile uint16_t PCDC_ToTargetCountO; // Request Count Out
static volatile uint8_t PCDC_ToTargetIdle;    // Request Idle  Flag

static uint8_t PCDC_ToTarget[UART_PACKET_COUNT][CDC_DATA_LEN];
static uint16_t PCDC_ToTargetSize[UART_PACKET_COUNT];

/* Staging copy for the in-flight UART write. R_SCI_UART_Write() is asynchronous
 * and does not take ownership, so the ring slot cannot be handed to it directly -
 * it would be recycled under the DMA/ISR. The g_sem_uart_tx semaphore is what
 * bounds this to one outstanding write, and UART_EVENT_TX_COMPLETE returns it. */
static uint8_t g_buf_uart_tx[CDC_DATA_LEN];

static usb_pcdc_ctrllinestate_t g_control_line_state = {
    .bdtr = 0,
    .brts = 0,
};

/* private function declarations */
static void handle_error(fsp_err_t err, char *err_str);
static void set_pcdc_line_coding(volatile usb_pcdc_linecoding_t *p_line_coding, const uart_cfg_t *p_uart_test_cfg);
static void set_uart_line_coding_cfg(uart_cfg_t *p_uart_test_cfg, const volatile usb_pcdc_linecoding_t *p_line_coding);
static fsp_err_t process_usb_events(usb_event_info_t *p_event_info);

static baud_setting_t baud_setting;
static bool enable_bitrate_modulation = true;

static uint32_t g_baud_rate = RESET_VALUE;
static uint32_t error_rate_x_1000 = BAUD_ERROR_RATE;
/* Working copy of g_uart_cfg. SET_LINE_CODING reopens SCI2 from this, so the
 * generated config in ra_gen/ stays pristine and a bad baud request from the host
 * cannot corrupt the defaults. sci_extend_cfg likewise shadows g_uart_cfg.p_extend
 * because g_uart_test_cfg.p_extend has to point at storage we own - it is where
 * the recalculated baud_setting lands. */
static uart_cfg_t g_uart_test_cfg;
static sci_uart_extended_cfg_t sci_extend_cfg;
/* Dead. Written in five places in the main loop (UART_TXING / UART_RXING) and read
 * by nobody - there is no reader anywhere in the firmware. It was presumably meant
 * to drive an activity LED; the status LED is driven from g_dap_bytes instead, so
 * this only tracks VCOM and would not have shown SWD traffic at all. Harmless, and
 * kept because removing it is a code change. */
static uint32_t g_uart_activity = 0x0;
static uint8_t g_PCDC_tx_data[CDC_DATA_LEN];

/* Pipe numbers are DISCOVERED at enumeration, not hardcoded.
 *
 * The composite device's endpoint-to-pipe assignment comes out of the FSP USB
 * stack and the descriptor set in src/r_usb_pcdc_pvnd_descriptor.c, and it is not
 * something this file gets to choose. So process_usb_events() walks pipes 1..9 at
 * USB_STATUS_CONFIGURED, asks R_USB_PipeInfoGet() about each, and claims the
 * first bulk OUT that is not the CDC one as cmd_pipe and the first two bulk INs
 * that are not the CDC one as rsp_pipe and swo_pipe.
 *
 * END_PIPE doubles as the "not found yet" sentinel, which is why these three
 * initialise to it: `== END_PIPE` is the claim test in that loop. They are also
 * non-static because swo_thread_entry.c refers to swo_pipe. */
#define START_PIPE (USB_PIPE1)   // Start pipe number
#define END_PIPE (USB_PIPE9 + 1) // Total pipe

uint8_t cmd_pipe = END_PIPE;
uint8_t rsp_pipe = END_PIPE;
uint8_t swo_pipe = END_PIPE;

/*******************************************************************************************************************/ /**
  *  @brief       Initialize line coding parameters based on UART configuration
  *  @param[in]   p_line_coding       Updates the line coding member values
                  p_uart_test_cfg     Pointer to store UART configuration properties
  *  @retval      None
  **********************************************************************************************************************/
static void set_pcdc_line_coding(volatile usb_pcdc_linecoding_t *p_line_coding, const uart_cfg_t *p_uart_test_cfg)
{
    /* Configure the line coding based on initial settings */
    if (g_uart_cfg.stop_bits == UART_STOP_BITS_1)
        p_line_coding->b_char_format = 0;
    else if (g_uart_cfg.stop_bits == UART_STOP_BITS_2)
        p_line_coding->b_char_format = 2;

    if (g_uart_cfg.parity == UART_PARITY_OFF)
        p_line_coding->b_parity_type = 0;
    else if (g_uart_cfg.parity == UART_PARITY_ODD)
        p_line_coding->b_parity_type = 1;
    else if (g_uart_cfg.parity == UART_PARITY_EVEN)
        p_line_coding->b_parity_type = 2;

    if (p_uart_test_cfg->data_bits == UART_DATA_BITS_8)
        p_line_coding->b_data_bits = 8;
    else if (p_uart_test_cfg->data_bits == UART_DATA_BITS_7)
        p_line_coding->b_data_bits = 7;
    else if (p_uart_test_cfg->data_bits == UART_DATA_BITS_9)
        p_line_coding->b_data_bits = 9;

    /* Ideally put the baud rate into p_line_coding;
     * but FSP does not have an API to calculate the baud-rate
     * based on the UART configuration values */
    ;
}

/*******************************************************************************************************************/ /**
  *  @brief       Initialize UART config values based on user input values through serial terminal
  *  @param[in]   p_uart_test_cfg   Pointer to store UART configuration properties
                  p_line_coding     Updates the line coding member values
  *  @retval      None
  **********************************************************************************************************************/
static void set_uart_line_coding_cfg(uart_cfg_t *p_uart_test_cfg, const volatile usb_pcdc_linecoding_t *p_line_coding)
{
    /* Set number of parity bits */
    switch (p_line_coding->b_parity_type)
    {
    default:
        p_uart_test_cfg->parity = UART_PARITY_OFF;
        break;
    case 1:
        p_uart_test_cfg->parity = UART_PARITY_ODD;
        break;
    case 2:
        p_uart_test_cfg->parity = UART_PARITY_EVEN;
        break;
    }
    /* Set number of data bits */
    switch (p_line_coding->b_data_bits)
    {
    default:
    case 8:
        p_uart_test_cfg->data_bits = UART_DATA_BITS_8;
        break;
    case 7:
        p_uart_test_cfg->data_bits = UART_DATA_BITS_7;
        break;
    case 9:
        p_uart_test_cfg->data_bits = UART_DATA_BITS_9;
        break;
    }
    /* Set number of stop bits */
    switch (p_line_coding->b_char_format)
    {
    default:
        p_uart_test_cfg->stop_bits = UART_STOP_BITS_1;
        break;
    case 2:
        p_uart_test_cfg->stop_bits = UART_STOP_BITS_2;
        break;
    }
}

/*******************************************************************************************************************/ /**
 *  @brief       Closes the USB and UART module , Print and traps error.
 *  @param[IN]   status    error status
 *  @param[IN]   err_str   error string

 *  @retval      None
 **********************************************************************************************************************/
static void handle_error(fsp_err_t err, char *err_str)
{
    if (FSP_SUCCESS != err)
    {
        if (FSP_SUCCESS != R_USB_Close(&g_basic1_ctrl))
        {
            APP_ERR_PRINT("\r\n** R_USB_Close API Failed ** \r\n ");
        }

        if (FSP_SUCCESS != R_SCI_UART_Close(&g_uart_ctrl))
        {
            APP_ERR_PRINT("\r\n**  R_SCI_UART_Close API failed  ** \r\n");
        }

        APP_PRINT(err_str);
        APP_ERR_TRAP(err);
    }
}

/*******************************************************************************************************************/ /**
  * @brief     Pump one queued SWO->USB request onto the SWO bulk IN pipe.
  *
  * CMSIS-DAP/SWO.c runs in the SWO thread but must not call R_USB_PipeWrite()
  * itself, so it posts SWO_USB_REQUEST records to g_queue_swo_usb and this thread
  * issues them. `{buf = NULL, num = 0}` is the agreed abort sentinel - it means
  * SWO_AbortTransfer() wants the pipe stopped rather than written.
  *
  * Non-blocking (timeout 0) and drains exactly one request per call, because it
  * sits in the main loop next to the DAP path and must not starve it. Depth is
  * bounded by the queue, and SWO.c keeps at most one transfer outstanding anyway.
  * @param[IN] None
  * @retval    None
  **********************************************************************************************************************/
void ProcessUsbSwoQueue(void)
{
    fsp_err_t err = FSP_SUCCESS;
    SWO_USB_REQUEST swo_usb_request;
    if (xQueueReceive(g_queue_swo_usb, &swo_usb_request, 0x0))
    {
        if ((swo_usb_request.buf == NULL) && (swo_usb_request.num == 0x0))
        {
            err = R_USB_PipeStop(&g_basic1_ctrl, swo_pipe);
            if (FSP_SUCCESS != err)
            {
                APP_ERR_PRINT("\r\nSWO_AbortTransfer R_USB_PipeStop API failed %d.\r\n", err);
            }
        }
        else
        {
            err = R_USB_PipeWrite(&g_basic1_ctrl, swo_usb_request.buf, swo_usb_request.num, swo_pipe);
            if (FSP_SUCCESS != err)
            {
                APP_ERR_PRINT("\r\nSWO_QueueTransfer R_USB_PipeWrite API failed %d.\r\n", err);
            }
        }
    }
}
/*******************************************************************************************************************/ /**
  * @brief     FreeRTOS entry point for the probe's main thread. Never returns.
  *
  * Setup, once: derive the USB serial number from the device UID, open USB, open
  * SCI2 for VCOM, sync the CDC line coding to it, and prime the two TX semaphores
  * so the first write in each direction can go out.
  *
  * Then loop forever, in this order:
  *   1. Drain g_queue_usb_event through process_usb_events(), and after each event
  *      run every DAP request the ring now holds to completion. That inner loop is
  *      the hot path - it can run for many milliseconds under a bulk transfer,
  *      which is why it updates the LED itself rather than waiting for step 5.
  *   2. VCOM host->target: one ring slot per pass, gated by g_sem_uart_tx.
  *   3. VCOM target->host: drain the RX queue into one bulk IN, gated by
  *      g_sem_pcdc_tx and by b_usb_configured.
  *   4. SWO, both halves (USB out, UART in).
  *   5. Status LED, then park on g_sem_DAP_Thread with a 1-tick timeout.
  *
  * The 1-tick timeout is deliberate: usb_composite_callback() gives the semaphore
  * on every USB event so real work wakes immediately, and the timeout only bounds
  * how stale the LED and the VCOM polls can get on a silent bus.
  *
  * @param[IN] pvParameters  Unused; required by the FreeRTOS task signature.
  * @retval    None. Does not return.
  **********************************************************************************************************************/
void dap_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);
    fsp_err_t err = FSP_SUCCESS;
    usb_event_info_t *p_event_info;

    bsp_unique_id_t const *p_uid = R_BSP_UniqueIdGet();
    char g_print_buffer[33];

    /* Enabled permanently for DAP and Led activity. */
    R_BSP_PinAccessEnable();

    /* Give the device a per-board USB serial number, from the 128-bit factory UID.
     *
     * The descriptor is a UTF-16LE string, so bLength - 2 bytes hold (bLength-2)/2
     * characters. Here bLength is STRING_DESCRIPTOR6_LEN = 28, giving maxIndex =
     * 13: only 13 of the 32 hex digits fit, i.e. the low nibble of UID byte 6
     * onwards is simply never shown. That is where a serial like "5196032D34385"
     * comes from - it is a truncation of the UID, not a hash, and two boards are
     * distinguishable only if they differ within those first 6.5 bytes. Widening it
     * means growing STRING_DESCRIPTOR6_LEN in src/r_usb_pcdc_pvnd_descriptor.c;
     * nothing here needs to change.
     *
     * The clamp below is in the wrong units - maxIndex counts characters, while
     * sizeof(unique_id_bytes) is 16 bytes and the real limit is the 32 hex digits
     * in g_print_buffer. It caps at half what it could, but it caps low, so it is
     * safe: with the current bLength of 28 it never fires at all. Left alone. */
    if (g_apl_string_descriptor_serial_number[0] >= 4)
    {
        uint32_t maxIndex = (uint32_t)((g_apl_string_descriptor_serial_number[0] - 2) / 2);

        if (maxIndex > sizeof(p_uid->unique_id_bytes))
        {
            /* Remaining id bytes are fixed */
            maxIndex = sizeof(p_uid->unique_id_bytes);
        }
        /* Update the USB Serial Number with the device UID. Writing only the even
         * bytes leaves the 0x00 high halves of the UTF-16LE pairs already in the
         * initialiser untouched, so the descriptor stays well-formed. */
        sprintf(g_print_buffer, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                p_uid->unique_id_bytes[0], p_uid->unique_id_bytes[1],
                p_uid->unique_id_bytes[2], p_uid->unique_id_bytes[3],
                p_uid->unique_id_bytes[4], p_uid->unique_id_bytes[5],
                p_uid->unique_id_bytes[6], p_uid->unique_id_bytes[7],
                p_uid->unique_id_bytes[8], p_uid->unique_id_bytes[9],
                p_uid->unique_id_bytes[10], p_uid->unique_id_bytes[11],
                p_uid->unique_id_bytes[12], p_uid->unique_id_bytes[13],
                p_uid->unique_id_bytes[14], p_uid->unique_id_bytes[15]);

        for (uint8_t index = 0; index < maxIndex; index++)
        {
            g_apl_string_descriptor_serial_number[2 + (index * 2)] = (uint8_t)g_print_buffer[index];
        }
    }

    /* Open USB instance */
    err = R_USB_Open(&g_basic1_ctrl, &g_basic1_cfg);
    if (FSP_SUCCESS != err)
    {
        handle_error(err, "\r\nR_USB_Open failed.\r\n");
    }
    APP_PRINT("\r\nUSB Opened successfully.\n\r");

    /* Open the UART with initial configuration.*/
    memcpy(&g_uart_test_cfg, &g_uart_cfg, sizeof(g_uart_test_cfg));
    memcpy(&sci_extend_cfg, g_uart_cfg.p_extend, sizeof(sci_extend_cfg));
    g_uart_test_cfg.p_extend = &sci_extend_cfg;
    err = R_SCI_UART_Open(&g_uart_ctrl, &g_uart_test_cfg);
    if (FSP_SUCCESS != err)
    {
        handle_error(err, "\r\n**  R_SCI_UART_Open API failed  **\r\n");
    }

    /* First time Synchronization of the line coding between UART & USB */
    set_pcdc_line_coding(&g_line_coding, &g_uart_test_cfg);

    /* Initialize the semaphores to allow one synchronization event to occur */
    {
        BaseType_t err_semaphore = xSemaphoreGive(g_sem_uart_tx);
        if (pdTRUE != err_semaphore)
        {
            handle_error(1, "\r\n xSemaphoreGive on g_sem_uart_tx Failed \r\n");
        }

        err_semaphore = xSemaphoreGive(g_sem_pcdc_tx);
        if (pdTRUE != err_semaphore)
        {
            handle_error(1, "\r\n xSemaphoreGive on g_sem_pcdc_tx Failed \r\n");
        }
    }

    while (true)
    {
        while (xQueueReceive(g_queue_usb_event, &p_event_info, 0x0))
        {
            uint32_t n;
            err = process_usb_events(p_event_info);

            // Process pending requests
            while (USB_RequestCountI != USB_RequestCountO)
            {

                /* Handle Queue Commands.
                 *
                 * ID_DAP_QueueCommands (0x7E) means "execute this packet, but do
                 * not send its response yet - more are coming". A host uses it to
                 * keep several packets in flight without waiting for each reply.
                 *
                 * The trick is that it is handled entirely by REWRITING the opcode:
                 * walk forward from the oldest unprocessed packet, turning every
                 * leading ID_DAP_QueueCommands into ID_DAP_ExecuteCommands (0x7F),
                 * and stop at the first packet that is not one (or at IndexI, the
                 * end of what has arrived). DAP.c then sees only ExecuteCommands
                 * and needs no queueing logic of its own.
                 *
                 * Note this rewrites the buffer in place and the outer loop then
                 * executes only USB_Request[USB_RequestIndexO]; the rest are left
                 * rewritten for the passes that follow. */
                n = USB_RequestIndexO;
                while (USB_Request[n][0] == ID_DAP_QueueCommands)
                {
                    USB_Request[n][0] = ID_DAP_ExecuteCommands;
                    n++;
                    if (n == DAP_PACKET_COUNT)
                    {
                        n = 0U;
                    }
                    if (n == USB_RequestIndexI)
                    {
                        break;
                    }
                }

                // Execute DAP Command (process request and prepare response)
                /* DAP_ExecuteCommand packs request bytes consumed in the upper
                 * half and response bytes produced in the lower half. */
                uint32_t executed =
                    DAP_ExecuteCommand(USB_Request[USB_RequestIndexO], USB_Response[USB_ResponseIndexI]);
                uint32_t req_bytes = executed >> 16;
                uint32_t rsp_bytes = executed & 0xFFFFU;

                USB_RespSize[USB_ResponseIndexI] = (uint16_t) rsp_bytes;

                /* Feeds the activity blank on the status LED. The larger half is
                 * the payload that was actually on the wire: a block read is
                 * 5 bytes in / ~1 KiB out, a block write the reverse. An add and
                 * a compare are nothing next to the SWD traffic just done. */
                g_dap_bytes += (req_bytes > rsp_bytes) ? req_bytes : rsp_bytes;

                /* Drive the LED from inside the command loop as well. Under a
                 * saturated transfer this loop can run for many milliseconds
                 * without the outer event loop turning over, and the blank
                 * timing has to stay honest regardless. It costs one tick read
                 * and a compare unless the level actually changes. */
                status_led_update();

                // Update Request Index and Count
                USB_RequestIndexO++;
                if (USB_RequestIndexO == DAP_PACKET_COUNT)
                {
                    USB_RequestIndexO = 0U;
                }
                USB_RequestCountO++;

                if (USB_RequestIdle)
                {
                    if ((uint16_t)(USB_RequestCountI - USB_RequestCountO) != DAP_PACKET_COUNT)
                    {
                        USB_RequestIdle = 0U;
                        err = R_USB_PipeRead(&g_basic1_ctrl, USB_Request[USB_RequestIndexI], DAP_PACKET_SIZE, cmd_pipe);
                        if (FSP_SUCCESS != err)
                        {
                            APP_ERR_PRINT("\r\nR_USB_PipeRead (USB_CLASS_PVND) failed.\r\n");
                        }
                    }
                }

                // Update Response Index and Count
                USB_ResponseIndexI++;
                if (USB_ResponseIndexI == DAP_PACKET_COUNT)
                {
                    USB_ResponseIndexI = 0U;
                }
                USB_ResponseCountI++;

                if (USB_ResponseIdle)
                {
                    if (USB_ResponseCountI != USB_ResponseCountO)
                    {
                        // Load data from response buffer to be sent back
                        n = USB_ResponseIndexO++;
                        if (USB_ResponseIndexO == DAP_PACKET_COUNT)
                        {
                            USB_ResponseIndexO = 0U;
                        }
                        USB_ResponseCountO++;
                        USB_ResponseIdle = 0U;
                        /* CMSIS-DAP command response to PC  */
                        err = R_USB_PipeWrite(&g_basic1_ctrl, USB_Response[n], USB_RespSize[n], rsp_pipe);
                        if (FSP_SUCCESS != err)
                        {
                            APP_ERR_PRINT("\r\nR_USB_PipeWrite API failed.\r\n");
                        }
                    }
                }
            }
        }

        /* Check if UART TX data has received from PC */
        if (PCDC_ToTargetCountI != PCDC_ToTargetCountO)
        {
            uint32_t n;
            g_uart_activity |= UART_TXING;
            if (xSemaphoreTake(g_sem_uart_tx, 0))
            {
                g_uart_activity |= UART_TXING;
                n = PCDC_ToTargetIndexO++;
                memcpy(g_buf_uart_tx, PCDC_ToTarget[n], PCDC_ToTargetSize[n]);
                err = R_SCI_UART_Write(&g_uart_ctrl, g_buf_uart_tx, PCDC_ToTargetSize[n]);
                if (FSP_SUCCESS != err)
                {
                    handle_error(err, "\r\n**  R_SCI_UART_Write API failed  **\r\n");
                }

                if (PCDC_ToTargetIndexO == UART_PACKET_COUNT)
                {
                    PCDC_ToTargetIndexO = 0U;
                }
                PCDC_ToTargetCountO++;

                if (PCDC_ToTargetIdle)
                {
                    if ((uint16_t)(PCDC_ToTargetCountI - PCDC_ToTargetCountO) != UART_PACKET_COUNT)
                    {
                        PCDC_ToTargetIdle = 0U;
                        err = R_USB_Read(&g_basic1_ctrl, PCDC_ToTarget[PCDC_ToTargetIndexI], CDC_DATA_LEN, USB_CLASS_PCDC);
                        if (FSP_SUCCESS != err)
                        {
                            APP_ERR_PRINT("\r\nR_USB_Read (USB_CLASS_PCDC) failed.\r\n");
                        }
                    }
                }
            }
        }
        else
        {
            g_uart_activity &= (uint32_t)~UART_TXING;
        }

        /* Check if UART RX data has received from target MCU */
        if (true == b_usb_configured)
        {
            QueueHandle_t *p_queue = (2 == g_uart_ctrl.data_bytes) ? &g_queue_uart_tx_16 : &g_queue_uart_tx_8;
            UBaseType_t msg_waiting_count = uxQueueMessagesWaiting(*p_queue);
            volatile uint32_t rx_data_size = (2 == g_uart_ctrl.data_bytes) ? 2 : 1;

            if (msg_waiting_count)
            {
                g_uart_activity |= UART_RXING;
            }
            else
            {
                g_uart_activity &= (uint32_t)~UART_RXING;
            }

            if (msg_waiting_count && xSemaphoreTake(g_sem_pcdc_tx, 0))
            {

                /* Pull out as many item from queue as possible */
                uint32_t unload_count = (msg_waiting_count < sizeof(g_PCDC_tx_data)) ? msg_waiting_count : sizeof(g_PCDC_tx_data);
                for (uint32_t itr = 0, idx = 0; itr < unload_count; itr++, idx += rx_data_size)
                {
                    if (pdTRUE != xQueueReceive(*p_queue, &g_PCDC_tx_data[idx], portMAX_DELAY))
                    {
                        handle_error(1, "\r\n Did not receive expected count of characters \r\n");
                    }
                }

                err = R_USB_Write(&g_basic1_ctrl, &g_PCDC_tx_data[0], unload_count * rx_data_size, USB_CLASS_PCDC);
                if (FSP_SUCCESS != err)
                {
                    handle_error(err, "\r\nR_USB_Write API failed.\r\n");
                }
            }
        }

        /* Check if UART SWO data has received from target MCU */
        ProcessUsbSwoQueue();
        ProcessUartSwoQueue();

        status_led_update();

        xSemaphoreTake(g_sem_DAP_Thread, 1);
    }
}

static fsp_err_t process_usb_events(usb_event_info_t *p_event_info)
{
    fsp_err_t err = FSP_SUCCESS;
    uint16_t used_pipe = 0;
    uint8_t pipe = 0;
    usb_pipe_t pipe_info;

    /* USB event received */
    switch (p_event_info->event)
    {
    case USB_STATUS_CONFIGURED: /* Configured State */
    {
        APP_PRINT("USB Configured Successfully\r\n");

        /* Discover which FSP pipes the enumerated endpoints landed on.
         *
         * R_USB_UsedPipesGet() returns a bitmask of pipes claimed by the vendor
         * class; each set bit is then queried for direction and transfer type. The
         * two CDC pipes are excluded explicitly by number (USB_CFG_PCDC_BULK_OUT /
         * _IN), so what is left is the CMSIS-DAP OUT and the two INs.
         *
         * Order matters and is fragile: rsp_pipe is claimed by the FIRST unclaimed
         * bulk IN and swo_pipe by the second, purely by ascending pipe number. That
         * is only correct because the descriptor set in
         * src/r_usb_pcdc_pvnd_descriptor.c lists the CMSIS-DAP IN endpoint before
         * the SWO one. Reorder those endpoints and SWO data goes out on the DAP
         * response pipe with no error anywhere - the APP_PRINT lines below are the
         * only witness, so check them if SWO ever comes back as garbage.
         *
         * Re-entry is safe: the `== END_PIPE` guards mean a second
         * SET_CONFIGURATION does not reshuffle pipes already claimed. */
        R_USB_UsedPipesGet(&g_basic1_ctrl, &used_pipe, USB_CLASS_PVND);
        for (pipe = START_PIPE; pipe < END_PIPE; pipe++)
        {
            if (0 != (used_pipe & (1 << pipe)))
            {
                R_USB_PipeInfoGet(&g_basic1_ctrl, &pipe_info, pipe);
                if (USB_EP_DIR_IN != (pipe_info.endpoint & USB_EP_DIR_IN))
                {
                    /* DAP Command Pipe */
                    if (USB_TRANSFER_TYPE_BULK == pipe_info.transfer_type)
                    {
                        if ((pipe != USB_CFG_PCDC_BULK_OUT) && (cmd_pipe == END_PIPE))
                        {
                            cmd_pipe = pipe;
                            APP_PRINT("\r\ncmd_pipe Pipe Number: %d Endpoint:0x%02X ", pipe, pipe_info.endpoint);
                        }
                    }
                }
                else
                {
                    /* DAP Command Response Pipe */
                    if (USB_TRANSFER_TYPE_BULK == pipe_info.transfer_type)
                    {
                        if ((pipe != USB_CFG_PCDC_BULK_IN) && (rsp_pipe == END_PIPE))
                        {
                            rsp_pipe = pipe;
                            APP_PRINT("\r\nrsp_pipe Pipe Number: %d Endpoint:0x%02X ", pipe, pipe_info.endpoint);
                        }
                        else if ((pipe != USB_CFG_PCDC_BULK_IN) && (swo_pipe == END_PIPE))
                        {
                            swo_pipe = pipe;
                            APP_PRINT("\r\nswo_pipe Pipe Number: %d Endpoint:0x%02X ", pipe, pipe_info.endpoint);
                        }
                    }
                }
            }
        }

        // Initialize variables
        USB_RequestIndexI = 0U;
        USB_RequestIndexO = 0U;
        USB_RequestCountI = 0U;
        USB_RequestCountO = 0U;
        USB_RequestIdle = 1U;

        USB_ResponseIndexI = 0U;
        USB_ResponseIndexO = 0U;
        USB_ResponseCountI = 0U;
        USB_ResponseCountO = 0U;
        USB_ResponseIdle = 1U;

        PCDC_ToTargetIndexI = 0U;
        PCDC_ToTargetIndexO = 0U;
        PCDC_ToTargetCountI = 0U;
        PCDC_ToTargetCountO = 0U;
        PCDC_ToTargetIdle = 1U;

        /* Read data from CMSIS-DAP Command pipe */
        USB_RequestIdle = 0U;
        err = R_USB_PipeRead(&g_basic1_ctrl, USB_Request[USB_RequestIndexI], DAP_PACKET_SIZE, cmd_pipe);
        if (FSP_SUCCESS != err)
        {
            APP_ERR_PRINT("\r\nR_USB_PipeRead (USB_CLASS_PVND) failed.\r\n");
        }

        /* Read data from serial port */
        PCDC_ToTargetIdle = 0U;
        err = R_USB_Read(&g_basic1_ctrl, PCDC_ToTarget[PCDC_ToTargetIndexI], CDC_DATA_LEN, USB_CLASS_PCDC);
        if (FSP_SUCCESS != err)
        {
            APP_ERR_PRINT("\r\nR_USB_Read (USB_CLASS_PCDC) failed.\r\n");
        }

        b_usb_configured = true;
        break;
    }

    case USB_STATUS_WRITE_COMPLETE: /* Write Complete State */
    {
        if (b_usb_configured)
        {
            switch (p_event_info->type)
            {
            case USB_CLASS_PCDC:
            {
                BaseType_t err_semaphore = xSemaphoreGive(g_sem_pcdc_tx);
                if (pdTRUE != err_semaphore)
                {
                    handle_error(1, "\r\n xSemaphoreGive on g_sem_pcdc_tx Failed \r\n");
                }
            }
            break;
            default:
            {
                if (rsp_pipe == p_event_info->pipe)
                {
                    /* Last write of CMSIS-DAP command response TO host machine has completed.
                       If we have another available, then that can be sent now otherwise we
                       set USB_ResponseIdle to indicate that we are free to send the next response
                       once one becomes available
                    */
                    if (USB_ResponseCountI != USB_ResponseCountO)
                    {
                        // Load data from response buffer to be sent back
                        err = R_USB_PipeWrite(&g_basic1_ctrl, USB_Response[USB_ResponseIndexO], USB_RespSize[USB_ResponseIndexO], rsp_pipe);
                        if (FSP_SUCCESS != err)
                        {
                            APP_ERR_PRINT("\r\nR_USB_PipeWrite API failed.\r\n");
                        }

                        USB_ResponseIndexO++;
                        if (USB_ResponseIndexO == DAP_PACKET_COUNT)
                        {
                            USB_ResponseIndexO = 0U;
                        }
                        USB_ResponseCountO++;
                    }
                    else
                    {
                        USB_ResponseIdle = 1U;
                    }
                }
                else if (swo_pipe == p_event_info->pipe)
                {
                    /* Let the SWO handler know the transfer is complete */
                    // APP_PRINT("process_usb_events USB_STATUS_WRITE_COMPLETE (SWO 0x%X)\r\n", swo_pipe);
                    SWO_TransferComplete();
                }
            }
            break;
            }
            if (FSP_SUCCESS != err)
            {
                APP_ERR_PRINT("\r\nR_USB_Read API failed.\r\n");
            }
        }
        break;
    }

    case USB_STATUS_READ_COMPLETE:
    {
        if (b_usb_configured)
        {
            switch (p_event_info->type)
            {
            case USB_CLASS_PCDC:
                PCDC_ToTargetSize[PCDC_ToTargetIndexI] = (uint16_t)p_event_info->data_size;
                PCDC_ToTargetIndexI++;
                if (PCDC_ToTargetIndexI == UART_PACKET_COUNT)
                {
                    PCDC_ToTargetIndexI = 0U;
                }
                PCDC_ToTargetCountI++;

                if ((uint16_t)(PCDC_ToTargetCountI - PCDC_ToTargetCountO) != UART_PACKET_COUNT)
                {
                    err = R_USB_Read(&g_basic1_ctrl, PCDC_ToTarget[PCDC_ToTargetIndexI], CDC_DATA_LEN, USB_CLASS_PCDC);
                    if (FSP_SUCCESS != err)
                    {
                        APP_ERR_PRINT("\r\nR_USB_Read (USB_CLASS_PCDC) failed.\r\n");
                    }
                }
                else
                {
                    PCDC_ToTargetIdle = 1U;
                }

                break;
            default:
                if (cmd_pipe == p_event_info->pipe)
                {
                    if (USB_Request[USB_RequestIndexI][0] == ID_DAP_TransferAbort)
                    {
                        DAP_TransferAbort = 1U;
                    }
                    else
                    {
                        USB_RequestIndexI++;
                        if (USB_RequestIndexI == DAP_PACKET_COUNT)
                        {
                            USB_RequestIndexI = 0U;
                        }
                        USB_RequestCountI++;
                    }
                }
                // Start reception of next request packet
                if ((uint16_t)(USB_RequestCountI - USB_RequestCountO) != DAP_PACKET_COUNT)
                {
                    err = R_USB_PipeRead(&g_basic1_ctrl, USB_Request[USB_RequestIndexI], DAP_PACKET_SIZE, cmd_pipe);
                    if (FSP_SUCCESS != err)
                    {
                        APP_ERR_PRINT("\r\nR_USB_PipeRead (USB_CLASS_PVND) failed.\r\n");
                    }
                }
                else
                {
                    USB_RequestIdle = 1U;
                }

                break;
            }
        }
        break;
    }

    case USB_STATUS_REQUEST: /* Receive Class Request */
    {
        /* Perform usb status request operation.*/
        /* Check for the specific CDC class request IDs */
        if (USB_PCDC_SET_LINE_CODING == (p_event_info->setup.request_type & USB_BREQUEST))
        {
            err = R_USB_PeriControlDataGet(&g_basic1_ctrl, (uint8_t *)&g_line_coding, LINE_CODING_LENGTH);
            if (FSP_SUCCESS == err)
            {
                /* Line Coding information read from the control pipe */
                sci_extend_cfg.p_baud_setting = &baud_setting;
                g_uart_test_cfg.p_extend = &sci_extend_cfg;

                /* Calculate the baud rate*/
                g_baud_rate = g_line_coding.dw_dte_rate;

                if (INVALID_SIZE < g_baud_rate)
                {
                    /* Calculate baud rate setting registers */
                    err = R_SCI_UART_BaudCalculate(g_baud_rate, enable_bitrate_modulation, error_rate_x_1000, &baud_setting);
                    if (FSP_SUCCESS != err)
                    {
                        handle_error(err, "\r\nR_SCI_UART_BaudCalculate failed.\r\n");
                    }
                }

                /* Set number of parity bits */
                set_uart_line_coding_cfg(&g_uart_test_cfg, &g_line_coding);
                /* Close module */
                err = R_SCI_UART_Close(&g_uart_ctrl);
                if (FSP_SUCCESS != err)
                {
                    APP_ERR_PRINT("\r\n**  R_SCI_UART_Close API failed  ** \r\n");
                }

                /* Open UART with changed UART settings */
                err = R_SCI_UART_Open(&g_uart_ctrl, &g_uart_test_cfg);
                if (FSP_SUCCESS != err)
                {
                    handle_error(err, "\r\nR_SCI_UART_Open failed.\r\n");
                }
            }
        }
        else if (USB_PCDC_GET_LINE_CODING == (p_event_info->setup.request_type & USB_BREQUEST))
        {
            /* Set the class request.*/
            err = R_USB_PeriControlDataSet(&g_basic1_ctrl, (uint8_t *)&g_line_coding, LINE_CODING_LENGTH);
            if (FSP_SUCCESS != err)
            {
                handle_error(err, "\r\nR_USB_PeriControlDataSet failed.\r\n");
            }
        }
        else if (USB_PCDC_SET_CONTROL_LINE_STATE == (p_event_info->setup.request_type & USB_BREQUEST))
        {
            /* Get the status of the control signals */
            err = R_USB_PeriControlDataGet(&g_basic1_ctrl,
                                           (uint8_t *)&g_control_line_state,
                                           sizeof(usb_pcdc_ctrllinestate_t));

            if (FSP_SUCCESS == err)
            {
                g_control_line_state.bdtr = (unsigned char)((p_event_info->setup.request_value >> 0) & 0x01);
                g_control_line_state.brts = (unsigned char)((p_event_info->setup.request_value >> 1) & 0x01);

                /* Toggle the line state if the flow control pin is set to a value (other than SCI_UART_INVALID_16BIT_PARAM) */
                if (SCI_UART_INVALID_16BIT_PARAM != g_uart_ctrl.flow_pin)
                {
                    R_BSP_PinWrite(g_uart_ctrl.flow_pin,
                                   (g_control_line_state.brts == 0) ? BSP_IO_LEVEL_LOW : BSP_IO_LEVEL_HIGH);
                }
            }

            /* Set the usb status as ACK response.*/
            err = R_USB_PeriControlStatusSet(&g_basic1_ctrl, USB_SETUP_STATUS_ACK);
            if (FSP_SUCCESS != err)
            {
                handle_error(err, "\r\nR_USB_PeriControlDataSet failed.\r\n");
            }
        }
        else if (USB_GET_DESCRIPTOR == (p_event_info->setup.request_type & USB_BREQUEST))
        {
            /* check for request value */
        }
        else if (USB_VENDOR_GET_MS_DESCRIPTOR_DEVICE == p_event_info->setup.request_type)
        {
            if (p_event_info->setup.request_index == EXT_COMPATID_OS_DESCRIPTOR)
            {
                /* Ignore interface number for the device */
                err = R_USB_PeriControlDataSet(&g_basic1_ctrl, (uint8_t *)&ecd, p_event_info->setup.request_length);
                if (FSP_SUCCESS != err)
                {
                    handle_error(err, "\r\nR_USB_PeriControlDataSet failed.\r\n");
                }
            }
        }
        else if (USB_VENDOR_GET_MS_DESCRIPTOR_INTERFACE == p_event_info->setup.request_type)
        {
            if (p_event_info->setup.request_index == EXT_PROP_OS_DESCRIPTOR)
            {
                if ((p_event_info->setup.request_value & 0x00FF) == INTERFACE_CMSIS_DAP)
                {
                    err = R_USB_PeriControlDataSet(&g_basic1_ctrl, ExtendedPropertiesDescriptor, p_event_info->setup.request_length);
                    if (FSP_SUCCESS != err)
                    {
                        handle_error(err, "\r\nR_USB_PeriControlDataSet failed.\r\n");
                    }
                }
            }
        }
        else
        {
            /* ACK all other status requests */
            err = R_USB_PeriControlStatusSet(&g_basic1_ctrl, USB_SETUP_STATUS_ACK);
            if (FSP_SUCCESS != err)
            {
                ;
            }
        }
        if (FSP_SUCCESS != err)
        {
            APP_ERR_PRINT("\r\nusb_status_request failed.\r\n");
        }
        break;
    }

    case USB_STATUS_REQUEST_COMPLETE: /* Request Complete State */
    {
        if (USB_PCDC_SET_LINE_CODING == (p_event_info->setup.request_type & USB_BREQUEST))
        {
            APP_PRINT("\nUSB STATUS : USB_STATUS_REQUEST_COMPLETE \nRequest_Type: USB_PCDC_SET_LINE_CODING \n");
        }
        else if (USB_PCDC_GET_LINE_CODING == (p_event_info->setup.request_type & USB_BREQUEST))
        {
            APP_PRINT("\nUSB STATUS : USB_STATUS_REQUEST_COMPLETE \nRequest_Type: USB_PCDC_GET_LINE_CODING \n");
        }
        else
        {
            // Do Nothing.
        }
        break;
    }

    case USB_STATUS_DETACH:
    {
        APP_PRINT("\nUSB STATUS : USB_STATUS_DETACH\r\n");
        /* VBUS is gone: the device really is unplugged and has lost its
         * address, its configuration and its pipes. */
        b_usb_configured = false;
        break;
    }

    case USB_STATUS_SUSPEND:
    {
        APP_PRINT("\nUSB STATUS : USB_STATUS_SUSPEND\r\n");
        /* Deliberately does NOT clear b_usb_configured.
         *
         * A suspend is the host idling the bus, not an unplug: the device
         * keeps its address, its configuration and its pipes across it, and
         * the host resumes without re-issuing SET_CONFIGURATION. Clearing the
         * flag here was therefore a one-way door - USB_STATUS_RESUME had
         * nothing to put it back - and this case used to share its body with
         * USB_STATUS_DETACH, so it did exactly that.
         *
         * The cost was not just cosmetic. Hosts suspend an idle interface
         * within seconds, so a probe that had enumerated perfectly well would
         * drop to the "never enumerated" slow blink and stay there for the
         * rest of the session, and the `if (b_usb_configured)` guards on the
         * read and write completion paths below would silently discard every
         * transfer that arrived afterwards. */
        break;
    }

    case USB_STATUS_RESUME:
    {
        APP_PRINT("\nUSB STATUS : USB_STATUS_RESUME\r\n");
        /* Nothing to restore, because the suspend no longer tears anything
         * down. Left explicit so the pair reads as a matched set. */
        break;
    }
    default:
    {
        break;
    }
    }
    return err;
}

/*******************************************************************************************************************/ /**
  * @brief       This function is callback for FreeRTOS+Composite.
  * @param[IN]   usb_event_info_t  *p_event_info
  * @param[IN]   usb_hdl_t         handler
  * @param[IN]   usb_onoff_t       on_off

  * @retval      None.
  **********************************************************************************************************************/
void usb_composite_callback(usb_event_info_t *p_event_info, usb_hdl_t handler, usb_onoff_t on_off)
{
    FSP_PARAMETER_NOT_USED(handler);
    FSP_PARAMETER_NOT_USED(on_off);

    /* The queue carries POINTERS, not copies - four bytes per entry, and
     * g_queue_usb_event is 20 entries deep (ra_gen/common_data.c).
     *
     * What they point into is FSP's own g_usb_cstd_event[], a 10-slot array that
     * usb_set_event() (ra/fsp/src/r_usb_basic/src/driver/r_usb_clibusbip.c) fills
     * round-robin: it writes slot `count`, calls this callback with
     * &g_usb_cstd_event[count], then does count = (count + 1) % USB_EVENT_MAX with
     * USB_EVENT_MAX = 10. The storage is therefore recycled after 10 further
     * events, whether or not we have read it.
     *
     * So the deeper 20-entry queue buys nothing: let 11 events land before the DAP
     * thread drains them and the oldest queued pointer now aliases a slot that has
     * been overwritten with a newer event, silently. In practice the thread is
     * woken by the give below and drains the queue in a tight while loop before
     * doing anything else, so the backlog stays at one or two - but it is why that
     * drain loop must stay ahead of everything else in the main loop, and why an
     * event body must not block.
     *
     * A zero-tick timeout is mandatory, not an optimisation: this runs in the USB
     * PCD task and blocking here would deadlock the stack against itself. */
    if (pdTRUE != (xQueueSend(g_queue_usb_event, (const void *)&p_event_info, (TickType_t)(RESET_VALUE))))
    {
        APP_ERR_PRINT("\r\n !! usb_composite_callback xQueueSend failed. \r\n");
    }

    /* Wake the DAP thread for EVERY USB event, not just SWO writes.
     *
     * The thread parks on xSemaphoreTake(g_sem_DAP_Thread, 1) and posting to
     * g_queue_usb_event does not unblock a task waiting on a semaphore, so
     * before this the arrival of a DAP command woke nothing: the thread slept
     * until the next SysTick. At configTICK_RATE_HZ = 1000 that is up to 1 ms,
     * 0.5 ms on average, added every time the request pipeline drains - and it
     * is a fixed wall-clock cost, so no amount of ICLK or SWD clock removes it.
     *
     * This runs in the PCD task's context rather than an ISR (usb_set_event ->
     * g_usb_apl_callback), so the non-FromISR give is the correct call. The
     * semaphore is binary, so a give while the DAP thread is already running is
     * retained and the next take returns immediately - no lost wakeup. */
    (void)xSemaphoreGive(g_sem_DAP_Thread);
}

/*******************************************************************************************************************/ /**
  *  @brief      UART user callback
  *  @param[in]  p_args

  *  @retval     None
  **********************************************************************************************************************/
void user_uart_callback(uart_callback_args_t *p_args)
{
    switch (p_args->event)
    {
    case UART_EVENT_RX_COMPLETE:
        break;
    case UART_EVENT_RX_CHAR:
    {
        sci_uart_instance_ctrl_t const *const p_ctrl = p_args->p_context;
        QueueHandle_t *p_queue = (2 == p_ctrl->data_bytes) ? &g_queue_uart_tx_16 : &g_queue_uart_tx_8;
        if (pdTRUE != (xQueueSendFromISR(*p_queue, &p_args->data, NULL)))
        {
            handle_error(1, "\r\n xQueueSend on g_queue_uart_tx Failed \r\n");
        }
    }
    break;
    /* Last byte is transmitting, ready for more data. */
    case UART_EVENT_TX_DATA_EMPTY:
        break;
    case UART_EVENT_TX_COMPLETE:
    {
        BaseType_t err_semaphore = xSemaphoreGiveFromISR(g_sem_uart_tx, NULL);
        if (pdTRUE != err_semaphore)
        {
            handle_error(1, "\r\n xSemaphoreGiveFromISR Failed \r\n");
        }
    }
    break;
    case UART_EVENT_ERR_PARITY:
        APP_ERR_PRINT("\r\n !! UART_EVENT_ERR_PARITY \r\n");
        break;
    case UART_EVENT_ERR_FRAMING:
        APP_ERR_PRINT("\r\n !! UART_EVENT_ERR_FRAMING \r\n");
        break;
    case UART_EVENT_ERR_OVERFLOW:
        APP_ERR_PRINT("\r\n !! UART_EVENT_ERR_OVERFLOW \r\n");
        break;
    case UART_EVENT_BREAK_DETECT:
        APP_ERR_PRINT("\r\n !! UART_EVENT_BREAK_DETECT \r\n");
        break;
    default: /** Do Nothing */
        break;
    }
}
