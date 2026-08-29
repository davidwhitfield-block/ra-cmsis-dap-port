/*
 * Copyright (c) 2013-2017 ARM Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ----------------------------------------------------------------------
 *
 * $Date:        1. December 2017
 * $Revision:    V2.0.0
 *
 * Project:      CMSIS-DAP Source
 * Title:        SW_DP.c CMSIS-DAP SW DP I/O
 *
 *---------------------------------------------------------------------------*/

#include "DAP_config.h"
#include "DAP.h"


// SW Macros

#define PIN_SWCLK_SET PIN_SWCLK_TCK_SET
#define PIN_SWCLK_CLR PIN_SWCLK_TCK_CLR

#define SW_CLOCK_CYCLE()                \
  PIN_SWCLK_CLR();                      \
  PIN_DELAY();                          \
  PIN_SWCLK_SET();                      \
  PIN_DELAY()

#define SW_WRITE_BIT(bit)               \
  PIN_SWDIO_OUT(bit);                   \
  PIN_SWCLK_CLR();                      \
  PIN_DELAY();                          \
  PIN_SWCLK_SET();                      \
  PIN_DELAY()

#define SW_READ_BIT(bit)                \
  PIN_SWCLK_CLR();                      \
  PIN_DELAY();                          \
  bit = PIN_SWDIO_IN();                 \
  PIN_SWCLK_SET();                      \
  PIN_DELAY()

#define PIN_DELAY() PIN_DELAY_SLOW(DAP_Data.clock_delay)


// Read the 32-bit RDATA phase of an SWD read, delay-free path only.
//
// The generic SW_READ_BIT loop costs 8 instructions per bit: two port stores and
// a port load (irreducible - they are the SWD clock and the sample), plus ubfx +
// lsl + orr to insert the bit, an add to accumulate parity, and subs/bne.
//
// Five of those go away. `lsrs` by DAP_SWDIO_BIT+1 drops SWDIO straight into the
// carry flag and `rrx` rotates it into the accumulator, doing in two instructions
// what ubfx/lsl/orr did in three and producing exactly the same LSB-first word as
// `val >>= 1; val |= bit << 31`. Parity is folded once from the finished word
// instead of being accumulated 32 times - the sum of the bits and their xor agree
// modulo 2, which is all the parity check looks at. An 8x unroll amortises the
// loop overhead to a quarter of an instruction per bit.
//
// Sampling is unchanged and still SWD-legal: the load sits between the store that
// drives SWCLK low and the store that drives SWCLK high, exactly where SW_READ_BIT
// put it, so the target is sampled in the SWCLK-low phase as before. The two ALU
// ops land after the SWCLK-high store, which is posted, so they drain in its
// shadow rather than adding to the period.
//
// Deliberately left in flash rather than .code_in_ram: at 8x the loop is ~100
// bytes and stays resident in the 256-byte FCACHE1, whereas SRAM0 and the I/O
// ports share the System bus, so executing from RAM would serialise instruction
// fetch against the very port traffic this loop is made of.
#define SWD_STR_(x) #x
#define SWD_STR(x)  SWD_STR_(x)

/* lsrs by this leaves SWDIO in C. The assembler needs a literal, so it is spelled
 * out and checked against the pin map rather than derived. */
#define SWD_SWDIO_CARRY_SHIFT 2
_Static_assert(SWD_SWDIO_CARRY_SHIFT == (int)(DAP_SWDIO_BIT + 1U),
               "SWD_SWDIO_CARRY_SHIFT must track DAP_SWDIO_BIT");
_Static_assert(DAP_SWCLK_BIT < 16U, "SWCLK must be in the low half of PCNTR3");

__attribute__((noinline))
static uint32_t SWD_ReadData32 (void) {
  volatile uint32_t *pidr = &DAP_SWD_PORT->PCNTR2;          /* PIDR  - sample     */
  volatile uint32_t *pctl = &DAP_SWD_PORT->PCNTR3;          /* POSR/PORR - SWCLK  */
  uint32_t lo  = 1UL << (DAP_SWCLK_BIT + 16U);              /* PORR: drive low    */
  uint32_t hi  = 1UL <<  DAP_SWCLK_BIT;                     /* POSR: drive high   */
  uint32_t v   = 0U;
  uint32_t cnt = 4U;
  uint32_t t;

  __asm volatile (
    ".syntax unified                                      \n"
    "1:                                                   \n"
    "   .rept 8                                           \n"
    "   str  %[lo], [%[pctl]]                             \n"  /* SWCLK low       */
    "   ldr  %[t],  [%[pidr]]                             \n"  /* sample SWDIO    */
    "   str  %[hi], [%[pctl]]                             \n"  /* SWCLK high      */
    "   lsrs %[t],  %[t], #" SWD_STR(SWD_SWDIO_CARRY_SHIFT) "\n" /* C = SWDIO     */
    "   rrx  %[v],  %[v]                                  \n"  /* val = C:val>>1  */
    "   .endr                                             \n"
    "   subs %[cnt], %[cnt], #1                           \n"
    "   bne  1b                                           \n"
    /* The "&" on v and cnt is load-bearing, not decoration. Without it GCC is
     * free to put an input in the same register as an output whenever it can see
     * the values are equal - and here cnt (4) and hi (1 << DAP_SWCLK_BIT = 4) are
     * equal, so it really does coalesce them. `subs cnt` then walks the SWCLK
     * mask down through 3, 2, 1: the second iteration would write 3 to PCNTR3,
     * driving SWO and SWDIO high instead of clocking SWCLK. Early-clobber says
     * these are written before the inputs are finished with, which forbids the
     * sharing. */
    : [v] "+&l" (v), [t] "=&l" (t), [cnt] "+&l" (cnt)
    : [pidr] "l" (pidr), [pctl] "l" (pctl), [lo] "l" (lo), [hi] "l" (hi)
    : "cc", "memory");

  return v;
}

/* Fold a finished word down to its parity in bit 0. */
#define SWD_PARITY32(p, v)                                                      \
  do {                                                                          \
    uint32_t p_ = (v) ^ ((v) >> 16);                                            \
    p_ ^= p_ >> 8;  p_ ^= p_ >> 4;                                              \
    p_ ^= p_ >> 2;  p_ ^= p_ >> 1;                                              \
    (p) = p_;                                                                   \
  } while (0)

/* The portable read used by the delay-inserting (Slow) instantiation. */
#define SW_READ_DATA32_GENERIC()                                                \
      val = 0U;                                                                 \
      parity = 0U;                                                              \
      for (n = 32U; n; n--) {                                                   \
        SW_READ_BIT(bit);               /* Read RDATA[0:31] */                  \
        parity += bit;                                                          \
        val >>= 1;                                                              \
        val  |= bit << 31;                                                      \
      }

#define SW_READ_DATA32() SW_READ_DATA32_GENERIC()


// Generate SWJ Sequence
//   count:  sequence bit count
//   data:   pointer to sequence bit data
//   return: none
#if ((DAP_SWD != 0) || (DAP_JTAG != 0))
void SWJ_Sequence (uint32_t count, const uint8_t *data) {
  uint32_t val;
  uint32_t n;

  val = 0U;
  n = 0U;
  while (count--) {
    if (n == 0U) {
      val = *data++;
      n = 8U;
    }
    if (val & 1U) {
      PIN_SWDIO_TMS_SET();
    } else {
      PIN_SWDIO_TMS_CLR();
    }
    SW_CLOCK_CYCLE();
    val >>= 1;
    n--;
  }
}
#endif


// Generate SWD Sequence
//   info:   sequence information
//   swdo:   pointer to SWDIO generated data
//   swdi:   pointer to SWDIO captured data
//   return: none
#if (DAP_SWD != 0)
void SWD_Sequence (uint32_t info, const uint8_t *swdo, uint8_t *swdi) {
  uint32_t val;
  uint32_t bit;
  uint32_t n, k;

  n = info & SWD_SEQUENCE_CLK;
  if (n == 0U) {
    n = 64U;
  }

  if (info & SWD_SEQUENCE_DIN) {
    while (n) {
      val = 0U;
      for (k = 8U; k && n; k--, n--) {
        SW_READ_BIT(bit);
        val >>= 1;
        val  |= bit << 7;
      }
      val >>= k;
      *swdi++ = (uint8_t)val;
    }
  } else {
    while (n) {
      val = *swdo++;
      for (k = 8U; k && n; k--, n--) {
        SW_WRITE_BIT(val);
        val >>= 1;
      }
    }
  }
}
#endif


#if (DAP_SWD != 0)


// SWD Transfer I/O
//   request: A[3:2] RnW APnDP
//   data:    DATA[31:0]
//   return:  ACK[2:0]
#define SWD_TransferFunction(speed)     /**/                                    \
static uint8_t SWD_Transfer##speed (uint32_t request, uint32_t *data) {         \
  uint32_t ack;                                                                 \
  uint32_t bit;                                                                 \
  uint32_t val;                                                                 \
  uint32_t parity;                                                              \
                                                                                \
  uint32_t n;                                                                   \
                                                                                \
  /* Packet Request */                                                          \
  parity = 0U;                                                                  \
  SW_WRITE_BIT(1U);                     /* Start Bit */                         \
  bit = request >> 0;                                                           \
  SW_WRITE_BIT(bit);                    /* APnDP Bit */                         \
  parity += bit;                                                                \
  bit = request >> 1;                                                           \
  SW_WRITE_BIT(bit);                    /* RnW Bit */                           \
  parity += bit;                                                                \
  bit = request >> 2;                                                           \
  SW_WRITE_BIT(bit);                    /* A2 Bit */                            \
  parity += bit;                                                                \
  bit = request >> 3;                                                           \
  SW_WRITE_BIT(bit);                    /* A3 Bit */                            \
  parity += bit;                                                                \
  SW_WRITE_BIT(parity);                 /* Parity Bit */                        \
  SW_WRITE_BIT(0U);                     /* Stop Bit */                          \
  SW_WRITE_BIT(1U);                     /* Park Bit */                          \
                                                                                \
  /* Turnaround */                                                              \
  PIN_SWDIO_OUT_DISABLE();                                                      \
  for (n = DAP_Data.swd_conf.turnaround; n; n--) {                              \
    SW_CLOCK_CYCLE();                                                           \
  }                                                                             \
                                                                                \
  /* Acknowledge response */                                                    \
  SW_READ_BIT(bit);                                                             \
  ack  = bit << 0;                                                              \
  SW_READ_BIT(bit);                                                             \
  ack |= bit << 1;                                                              \
  SW_READ_BIT(bit);                                                             \
  ack |= bit << 2;                                                              \
                                                                                \
  if (ack == DAP_TRANSFER_OK) {         /* OK response */                       \
    /* Data transfer */                                                         \
    if (request & DAP_TRANSFER_RnW) {                                           \
      /* Read data */                                                           \
      SW_READ_DATA32();                 /* Read RDATA[0:31] */                  \
      SW_READ_BIT(bit);                 /* Read Parity */                       \
      if ((parity ^ bit) & 1U) {                                                \
        ack = DAP_TRANSFER_ERROR;                                               \
      }                                                                         \
      if (data) { *data = val; }                                                \
      /* Turnaround */                                                          \
      for (n = DAP_Data.swd_conf.turnaround; n; n--) {                          \
        SW_CLOCK_CYCLE();                                                       \
      }                                                                         \
      PIN_SWDIO_OUT_ENABLE();                                                   \
    } else {                                                                    \
      /* Turnaround */                                                          \
      for (n = DAP_Data.swd_conf.turnaround; n; n--) {                          \
        SW_CLOCK_CYCLE();                                                       \
      }                                                                         \
      PIN_SWDIO_OUT_ENABLE();                                                   \
      /* Write data */                                                          \
      val = *data;                                                              \
      parity = 0U;                                                              \
      for (n = 32U; n; n--) {                                                   \
        SW_WRITE_BIT(val);              /* Write WDATA[0:31] */                 \
        parity += val;                                                          \
        val >>= 1;                                                              \
      }                                                                         \
      SW_WRITE_BIT(parity);             /* Write Parity Bit */                  \
    }                                                                           \
    /* Capture Timestamp */                                                     \
    if (request & DAP_TRANSFER_TIMESTAMP) {                                     \
      DAP_Data.timestamp = TIMESTAMP_GET();                                     \
    }                                                                           \
    /* Idle cycles */                                                           \
    n = DAP_Data.transfer.idle_cycles;                                          \
    if (n) {                                                                    \
      PIN_SWDIO_OUT(0U);                                                        \
      for (; n; n--) {                                                          \
        SW_CLOCK_CYCLE();                                                       \
      }                                                                         \
    }                                                                           \
    PIN_SWDIO_OUT(1U);                                                          \
    return ((uint8_t)ack);                                                      \
  }                                                                             \
                                                                                \
  if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {              \
    /* WAIT or FAULT response */                                                \
    if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) != 0U)) { \
      for (n = 32U+1U; n; n--) {                                                \
        SW_CLOCK_CYCLE();               /* Dummy Read RDATA[0:31] + Parity */   \
      }                                                                         \
    }                                                                           \
    /* Turnaround */                                                            \
    for (n = DAP_Data.swd_conf.turnaround; n; n--) {                            \
      SW_CLOCK_CYCLE();                                                         \
    }                                                                           \
    PIN_SWDIO_OUT_ENABLE();                                                     \
    if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) == 0U)) { \
      PIN_SWDIO_OUT(0U);                                                        \
      for (n = 32U+1U; n; n--) {                                                \
        SW_CLOCK_CYCLE();               /* Dummy Write WDATA[0:31] + Parity */  \
      }                                                                         \
    }                                                                           \
    PIN_SWDIO_OUT(1U);                                                          \
    return ((uint8_t)ack);                                                      \
  }                                                                             \
                                                                                \
  /* Protocol error */                                                          \
  for (n = DAP_Data.swd_conf.turnaround + 32U + 1U; n; n--) {                   \
    SW_CLOCK_CYCLE();                   /* Back off data phase */               \
  }                                                                             \
  PIN_SWDIO_OUT_ENABLE();                                                       \
  PIN_SWDIO_OUT(1U);                                                            \
  return ((uint8_t)ack);                                                        \
}


/* Fast: no inter-edge delay, so the hand-rolled 32-bit read applies. */
#undef  PIN_DELAY
#define PIN_DELAY() PIN_DELAY_FAST()
#undef  SW_READ_DATA32
#define SW_READ_DATA32()                                                        \
      val = SWD_ReadData32();                                                   \
      SWD_PARITY32(parity, val);
SWD_TransferFunction(Fast)

/* Slow: every edge must carry PIN_DELAY_SLOW to honour a sub-maximum clock
 * request, so this one keeps the portable bit loop. */
#undef  PIN_DELAY
#define PIN_DELAY() PIN_DELAY_SLOW(DAP_Data.clock_delay)
#undef  SW_READ_DATA32
#define SW_READ_DATA32() SW_READ_DATA32_GENERIC()
SWD_TransferFunction(Slow)


// SWD Transfer I/O
//   request: A[3:2] RnW APnDP
//   data:    DATA[31:0]
//   return:  ACK[2:0]
uint8_t  SWD_Transfer(uint32_t request, uint32_t *data) {
  if (DAP_Data.fast_clock) {
    return SWD_TransferFast(request, data);
  } else {
    return SWD_TransferSlow(request, data);
  }
}


#endif  /* (DAP_SWD != 0) */
