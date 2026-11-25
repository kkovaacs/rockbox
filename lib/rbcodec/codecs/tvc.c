/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Videoton TV Computer CAS tape file codec
 *
 * Copyright (c) 2012 Krisztian Kovacs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "debug.h"
#include "codeclib.h"
#include <inttypes.h>

#include "logf.h"

CODEC_HEADER

#define CHUNK_SIZE 1024
#define CAS_DATA_OFFSET 0x90 /* UPM header + program header */
/* headers plus the largest program the 16-bit length field can describe */
#define CAS_MAX_FILESIZE (CAS_DATA_OFFSET + 0xffff)
#define SAMPLE_RATE 44100

#define SILENCE 0x0000
#define POS_PEAK 0x7d00
#define NEG_PEAK -0x7d00

/* Periods per the TVC tape format, rounded to whole samples at 44.1 kHz */

/* "0" bit: 552 us, 1812 Hz -> 24.3 samples */
static const int16_t zero_bit[24] ICONST_ATTR = {
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
};

/* "1" bit: 388 us, 2577 Hz -> 17.1 samples */
static const int16_t one_bit[17] ICONST_ATTR = {
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
};

/* leader tone: 470 us, 2128 Hz -> 20.7 samples */
static const int16_t pre[21] ICONST_ATTR = {
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK,
};

/* sync: 736 us, 1359 Hz -> 32.5 samples */
static const int16_t sync_samples[32] ICONST_ATTR = {
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
};

#define DIM(array) (sizeof(array) / sizeof(array[0]))

static int16_t samples[CHUNK_SIZE] IBSS_ATTR;
static unsigned int samples_written;
static uint16_t current_crc;

/* playback position, in samples generated since the start of the tape */
static unsigned long position;
/* samples before this position are generated but not played (seeking) */
static unsigned long skip_until;
/* stop generating: playback halted, or restarting for a backward seek */
static bool stop;
static bool restart;

/* feed one bit into the checksum, as the TVC ROM routine does */
static uint16_t crc_update(uint16_t crc, unsigned char bit)
{
    unsigned char a, carry;

    a = bit ? 0x80 : 0;
    a ^= crc >> 8;
    carry = a & 0x80;
    if (carry)
        crc ^= 0x0810;
    crc <<= 1;
    if (carry)
        crc++;

    return crc;
}

static unsigned long ms_to_samples(unsigned long ms)
{
    return (uint64_t)ms * SAMPLE_RATE / 1000;
}

static void handle_command(void)
{
    intptr_t param;
    enum codec_command_action action = ci->get_command(&param);

    if (action == CODEC_ACTION_HALT) {
        stop = true;
    } else if (action == CODEC_ACTION_SEEK_TIME) {
        /* the tape is generated sequentially: skip forward to the target,
           or start over and skip to it when seeking backwards */
        skip_until = ms_to_samples(param);
        if (skip_until < position) {
            restart = true;
            stop = true;
        }
        ci->set_elapsed(param);
        ci->seek_complete();
    }
}

/* send generated samples to the PCM buffer, unless they are being skipped */
static void insert(int16_t const *buf, unsigned int num)
{
    if (stop)
        return;

    if (position + num <= skip_until) {
        position += num;
    } else {
        if (position < skip_until) {
            unsigned int skip = skip_until - position;
            buf += skip;
            num -= skip;
            position += skip;
        }

        ci->pcmbuf_insert(buf, NULL, num);
        position += num;
        ci->set_elapsed((uint64_t)position * 1000 / SAMPLE_RATE);
    }

    handle_command();
}

/* send the samples collected by output() to the PCM buffer */
static void flush(void)
{
    if (samples_written > 0) {
        insert(samples, samples_written);
        samples_written = 0;
    }
}

static void output(int16_t const *sample, unsigned int num)
{
    if (samples_written + num > CHUNK_SIZE)
        flush();

    memcpy(samples + samples_written, sample, num * sizeof(int16_t));
    samples_written += num;
}

static void out_silence_1s(void)
{
    unsigned int i = SAMPLE_RATE;

    flush(); /* samples[] is reused below */
    memset(samples, 0, sizeof(samples));

    while (i > 0) {
        unsigned int len = MIN(i, DIM(samples));
        insert(samples, len);
        i -= len;
    }
}

static void copy_sample_buffer(int16_t const *sample,
                               size_t len,
                               unsigned int num)
{
    unsigned int i;

    for (i = 0; i < num * len; i += len)
        memcpy(((char *)samples) + i, sample, len);
}

static size_t fill_sample_buffer(int16_t const *sample, size_t len)
{
    size_t num_to_fill = sizeof(samples) / len;

    copy_sample_buffer(sample, len, num_to_fill);

    return num_to_fill * len;
}

static void out_pre(unsigned int num)
{
    size_t length;
    unsigned int num_in_buffer;

    flush(); /* samples[] is reused below */
    length = fill_sample_buffer(pre, sizeof(pre));
    num_in_buffer = length / sizeof(pre);

    while (num > 0) {
        unsigned int i = MIN(num, num_in_buffer);
        insert(samples, i * DIM(pre));
        num -= i;
    }
}

static void out_sync(void)
{
    output(sync_samples, DIM(sync_samples));
}

static void out_bit(unsigned char bit)
{
    if (bit == 0)
        output(zero_bit, DIM(zero_bit));
    else
        output(one_bit, DIM(one_bit));

    current_crc = crc_update(current_crc, bit);
}

static void out_byte(unsigned char data)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        out_bit(data & 1);
        data >>= 1;
    }
}

static void out_word(uint16_t data)
{
    out_byte(data & 0xff);
    out_byte(data >> 8);
}

/* name in the tape header; the TVC allows at most 10 characters */
#define TAPE_NAME "TEST"
#define TAPE_NAME_LEN (sizeof(TAPE_NAME) - 1)
typedef char tape_name_fits[TAPE_NAME_LEN <= 10 ? 1 : -1];

static void out_string(const char *str, size_t len)
{
    unsigned int i;

    out_byte(len);
    for (i = 0; i < len; i++)
        out_byte(str[i]);
}

static void out_headblock(unsigned char *casfile)
{
    unsigned int i;

    out_silence_1s();
    out_silence_1s();

    out_pre(10240);
    out_sync();

    out_byte(0);    // sync
    current_crc = 0;
    out_byte(0x6a); // sync

    out_byte(0xff); // header block
    out_byte(0x11); // non-buffered
    out_byte(0);    // not write-protected
    out_byte(1);    // only one sector in the head block
    out_byte(0);    // sector number, always zero for head block
    // bytes in sector: file name and non-buffered block header
    out_byte(1 + TAPE_NAME_LEN + 16);

    out_string(TAPE_NAME, TAPE_NAME_LEN); // file name

    // CAS block header
    for (i = 0x80; i < 0x90; i++)
        out_byte(casfile[i]);

    out_byte(0x00); // file end mark
    out_word(current_crc);
    out_pre(5);
}

static void out_datahead(size_t len)
{
    out_silence_1s();

    out_pre(5120);
    out_sync();

    out_byte(0);    // sync
    current_crc = 0;
    out_byte(0x6a); // sync

    out_byte(0x00); // data block
    out_byte(0x11); // non-buffered
    out_byte(0);    // not write-protected
    out_byte((len + 255) / 256); // sectors in this block, 0 means 256
}

static void out_datablock(unsigned char *data, size_t len, unsigned int sector,
                          bool last)
{
    if (sector > 1)
        current_crc = 0;

    out_byte(sector);
    out_byte(len & 0xffLL);

    for (; len > 0; len--, data++)
        out_byte(*data);

    out_byte(last ? 0xff : 0x00); // file end mark, included in the CRC
    out_word(current_crc);
}

static void out_data(unsigned char *casfile, size_t data_length)
{
    unsigned int i;
    size_t offset;

    /* a block can't have zero sectors: an empty program has no data block */
    if (data_length > 0) {
        out_datahead(data_length);

        for (i = 0, offset = 0; offset < data_length; i++, offset += 256) {
            size_t chunk_length = MIN(256, data_length - offset);
            out_datablock(casfile + CAS_DATA_OFFSET + offset, chunk_length,
                          i + 1, offset + chunk_length >= data_length);
        }

        out_pre(5);
    }

    out_silence_1s();
    out_silence_1s();
}

enum codec_status codec_main(enum codec_entry_call_reason reason)
{
    if (reason == CODEC_LOAD) {
        ci->configure(DSP_SET_FREQUENCY, SAMPLE_RATE);
        ci->configure(DSP_SET_SAMPLE_DEPTH, 16);
        ci->configure(DSP_SET_STEREO_MODE, STEREO_MONO);
    }

    return CODEC_OK;
}

enum codec_status codec_run(void)
{
    size_t filesize, wanted;
    unsigned char *casfile = NULL;
    size_t program_length;

    if (codec_init()) {
        return CODEC_ERROR;
    }

    codec_set_replaygain(ci->id3);

    /* the whole file (or as much as can hold a program) must be in one
       contiguous buffer; request_buffer may return less than asked for */
    wanted = MIN((size_t)ci->filesize, (size_t)CAS_MAX_FILESIZE);

    ci->seek_buffer(0);
    casfile = ci->request_buffer(&filesize, wanted);

    if (casfile == NULL || filesize < wanted) {
        DEBUGF("CODEC_ERROR: could not buffer CAS file\n");
        return CODEC_ERROR;
    }

    if (filesize < CAS_DATA_OFFSET) {
        DEBUGF("CODEC_ERROR: CAS file too short: %u\n", (unsigned int)filesize);
        return CODEC_ERROR;
    }

    /* check UPM header */
    if (casfile[0] != 0x11) {
        DEBUGF("CODEC_ERROR: not a non-buffered CAS file: %u\n", (unsigned int)casfile[0]);
        return CODEC_ERROR;
    }

    /* program length from the non-buffered file header */
    program_length = (casfile[0x83] << 8) + casfile[0x82];

    /* the tape header repeats this length, so it can't be shortened */
    if (program_length > filesize - CAS_DATA_OFFSET) {
        DEBUGF("CODEC_ERROR: CAS program size %u exceeds file\n",
               (unsigned int)program_length);
        return CODEC_ERROR;
    }

    DEBUGF("CAS type %u, program size %u, autorun %u, version %u\n",
           casfile[0x81], (unsigned int)program_length,
           casfile[0x84], casfile[0x8f]);

    /* resume where playback was left off */
    skip_until = ms_to_samples(ci->id3->elapsed);

    do {
        /* the codec stays loaded across tracks of the same type, and a
           backward seek starts the tape over */
        samples_written = 0;
        current_crc = 0;
        position = 0;
        stop = false;
        restart = false;

        out_headblock(casfile);
        out_data(casfile, program_length);
        flush();
    } while (restart);

    return CODEC_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-default-style: linux
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
