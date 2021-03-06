/*
 * Copyright (c) 2009 Corey Tabaka
 * Copyright (c) 2015 Intel Corporation
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <sys/types.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/reg.h>
#include <assert.h>
#include <kernel/thread.h>
#include <platform/interrupts.h>
#include <arch/ops.h>
#include <arch/x86.h>
#include <kernel/spinlock.h>
#include "platform_p.h"
#include <platform/pc.h>

static spin_lock_t lock;

#define PIC1 0x20
#define PIC2 0xA0

#define ICW1 0x11
#define ICW4 0x01

struct int_handler_struct {
    int_handler handler;
    void *arg;
};

static struct int_handler_struct int_handler_table[INT_VECTORS];

/*
 * Cached IRQ mask (enabled/disabled)
 */
static uint8_t irqMask[2];

/*
 * init the PICs and remap them
 */
static void map(uint32_t pic1, uint32_t pic2) {
    /* send ICW1 */
    outp(PIC1, ICW1);
    outp(PIC2, ICW1);

    /* send ICW2 */
    outp(PIC1 + 1, pic1);   /* remap */
    outp(PIC2 + 1, pic2);   /*  pics */

    /* send ICW3 */
    outp(PIC1 + 1, 4);  /* IRQ2 -> connection to slave */
    outp(PIC2 + 1, 2);

    /* send ICW4 */
    outp(PIC1 + 1, 5);
    outp(PIC2 + 1, 1);

    /* disable all IRQs */
    outp(PIC1 + 1, 0xff);
    outp(PIC2 + 1, 0xff);

    irqMask[0] = 0xff;
    irqMask[1] = 0xff;
}

static void enable(unsigned int vector, bool enable) {
    if (vector >= PIC1_BASE && vector < PIC1_BASE + 8) {
        vector -= PIC1_BASE;

        uint8_t bit = 1 << vector;

        if (enable && (irqMask[0] & bit)) {
            irqMask[0] = inp(PIC1 + 1);
            irqMask[0] &= ~bit;
            outp(PIC1 + 1, irqMask[0]);
            irqMask[0] = inp(PIC1 + 1);
        } else if (!enable && !(irqMask[0] & bit)) {
            irqMask[0] = inp(PIC1 + 1);
            irqMask[0] |= bit;
            outp(PIC1 + 1, irqMask[0]);
            irqMask[0] = inp(PIC1 + 1);
        }
    } else if (vector >= PIC2_BASE && vector < PIC2_BASE + 8) {
        vector -= PIC2_BASE;

        uint8_t bit = 1 << vector;

        if (enable && (irqMask[1] & bit)) {
            irqMask[1] = inp(PIC2 + 1);
            irqMask[1] &= ~bit;
            outp(PIC2 + 1, irqMask[1]);
            irqMask[1] = inp(PIC2 + 1);
        } else if (!enable && !(irqMask[1] & bit)) {
            irqMask[1] = inp(PIC2 + 1);
            irqMask[1] |= bit;
            outp(PIC2 + 1, irqMask[1]);
            irqMask[1] = inp(PIC2 + 1);
        }

        bit = 1 << (INT_PIC2 - PIC1_BASE);

        if (irqMask[1] != 0xff && (irqMask[0] & bit)) {
            irqMask[0] = inp(PIC1 + 1);
            irqMask[0] &= ~bit;
            outp(PIC1 + 1, irqMask[0]);
            irqMask[0] = inp(PIC1 + 1);
        } else if (irqMask[1] == 0 && !(irqMask[0] & bit)) {
            irqMask[0] = inp(PIC1 + 1);
            irqMask[0] |= bit;
            outp(PIC1 + 1, irqMask[0]);
            irqMask[0] = inp(PIC1 + 1);
        }
    } else {
        //dprintf(DEBUG, "Invalid PIC interrupt: %02x\n", vector);
    }
}

static void issueEOI(unsigned int vector) {
    if (vector >= PIC1_BASE && vector <= PIC1_BASE + 7) {
        outp(PIC1, 0x20);
    } else if (vector >= PIC2_BASE && vector <= PIC2_BASE + 7) {
        outp(PIC2, 0x20);
        outp(PIC1, 0x20);   // must issue both for the second PIC
    }
}

void platform_init_interrupts(void) {
    // rebase the PIC out of the way of processor exceptions
    map(PIC1_BASE, PIC2_BASE);
}

status_t mask_interrupt(unsigned int vector) {
    if (vector >= INT_VECTORS)
        return ERR_INVALID_ARGS;

//  dprintf(DEBUG, "%s: vector %d\n", __PRETTY_FUNCTION__, vector);

    spin_lock_saved_state_t state;
    spin_lock_irqsave(&lock, state);

    enable(vector, false);

    spin_unlock_irqrestore(&lock, state);

    return NO_ERROR;
}


static void platform_mask_irqs(void) {
    irqMask[0] = inp(PIC1 + 1);
    irqMask[1] = inp(PIC2 + 1);

    outp(PIC1 + 1, 0xff);
    outp(PIC2 + 1, 0xff);

    irqMask[0] = inp(PIC1 + 1);
    irqMask[1] = inp(PIC2 + 1);
}

status_t unmask_interrupt(unsigned int vector) {
    if (vector >= INT_VECTORS)
        return ERR_INVALID_ARGS;

//  dprintf("%s: vector %d\n", __PRETTY_FUNCTION__, vector);

    spin_lock_saved_state_t state;
    spin_lock_irqsave(&lock, state);

    enable(vector, true);

    spin_unlock_irqrestore(&lock, state);

    return NO_ERROR;
}

enum handler_return platform_irq(x86_iframe_t *frame);
enum handler_return platform_irq(x86_iframe_t *frame) {
    // get the current vector
    unsigned int vector = frame->vector;

    DEBUG_ASSERT(vector >= 0x20);

    // deliver the interrupt
    enum handler_return ret = INT_NO_RESCHEDULE;

    if (int_handler_table[vector].handler)
        ret = int_handler_table[vector].handler(int_handler_table[vector].arg);

    // ack the interrupt
    issueEOI(vector);

    return ret;
}

void register_int_handler(unsigned int vector, int_handler handler, void *arg) {
    if (vector >= INT_VECTORS)
        panic("register_int_handler: vector out of range %d\n", vector);

    spin_lock_saved_state_t state;
    spin_lock_irqsave(&lock, state);

    int_handler_table[vector].arg = arg;
    int_handler_table[vector].handler = handler;

    spin_unlock_irqrestore(&lock, state);
}
