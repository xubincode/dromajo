//******************************************************************************
// Copyright (C) 2018,2019, Esperanto Technologies Inc.
// The copyright to the computer program(s) herein is the
// property of Esperanto Technologies, Inc. All Rights Reserved.
// The program(s) may be used and/or copied only with
// the written permission of Esperanto Technologies and
// in accordance with the terms and conditions stipulated in the
// agreement/contract under which the program(s) have been supplied.
//------------------------------------------------------------------------------

/*
 * RISCV CPU emulator
 *
 * Copyright (c) 2018,2019 Esperanto Technologies
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef VALIDATION_EVENTS_H
#define VALIDATION_EVENTS_H

#include <stdint.h>
#include <stdbool.h>

// List of different validation commands passed
// to validation CSR 1

#define CMD_OFFSET 56
// Maxion owns commands 128 upwards
#define CMD_PREFIX 0x80ULL
#define CMD_MASK (~CMD_PREFIX & 0xFF)
#define PAYLOAD_OFFSET 0
#define PAYLOAD_MASK ((1ULL << CMD_OFFSET) - 1)

/* List of different validation commands
 */
enum VALIDATON_CMD {
    VALIDATION_CMD_INVALID = CMD_PREFIX,
    VALIDATION_CMD_LINUX, 	// Linux events
    VALIDATION_CMD_BENCH, 	// Benchmark events
    VALIDATION_CMD_EXIT_CODE, 	// The binary return value
};

/* List of Linux related events
 */
enum LINUX_CMD_VALUE {
    LINUX_CMD_VALUE_INVALID = 0,
    LINUX_CMD_VALUE_BOOT_DONE, 	// We are done booting
    LINUX_CMD_VALUE_TERMINATE, 	// Terminate boot, this is a workaround for halt
    LINUX_CMD_VALUE_NUM
};

/* List of benchmark related events
 */
enum BENCH_CMD_VALUE {
    BENCH_CMD_VALUE_INVALID = 0,
    BENCH_CMD_VALUE_START, 	// Benchmark start
    BENCH_CMD_VALUE_END, 	// Benchmark end
    BENCH_CMD_VALUE_NUM
};

#define EVENT(CMD_NAME, PAYLOAD) (((uint64_t)VALIDATION_CMD_ ## CMD_NAME << CMD_OFFSET) | CMD_NAME ## _CMD_VALUE_ ## PAYLOAD)

struct event_info
{
    uint64_t value; // Full value
    const char *name;
    bool terminate;
};

/* List of the different validation events that we can recognize
 */
static const struct event_info validation_events[] = {
    { EVENT(LINUX, BOOT_DONE), "linux-boot",      true },
    { EVENT(LINUX, TERMINATE), "linux-terminate", true },
    { EVENT(BENCH, START),     "benchmark-start", true },
    { EVENT(BENCH, END),       "benchmark-end",   true },
};

#endif // VALIDATION_EVENTS_H
