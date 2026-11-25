/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Videoton TV Computer CAS tape file metadata parser
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

#include "system.h"
#include "metadata.h"
#include "metadata_common.h"
#include "metadata_parsers.h"

#include "logf.h"

/*
 TVC "0" bit, 552 usec: 12 high 12 low samples at 44.1 kHz
 TVC "1" bit, 388 usec: 9 high 8 low

 TVC pre sound, 470 usec, 10 high 11 low
 TVC syncrone, 736 usec, 16 high 16 low

 TVC Headblock
 0) 2 sec silence
 1) 10240 pre sound
 2) 1 syncrone
 3) Head data
 4) 5 pre sound

 TVC Datablock
 0) 1 sec silence
 1) 5120 pre sound
 2) 1 syncrone
 3) Data data
 4) 5 pre sound

 1 sec silence =  44100 silence
*/
#define CAS_DATA_OFFSET 0x90 /* UPM header + program header */

bool get_tvc_metadata(int fd, struct mp3entry *id3)
{
    unsigned char header[CAS_DATA_OFFSET];
    unsigned long program_length;
    unsigned long samples;

    logf("Getting TVC metadata");

    /* .cas is also used by other machines (MSX, TRS-80, ...): accept only
       non-buffered TVC files whose program fits in the file */
    if (lseek(fd, 0, SEEK_SET) < 0 ||
        read(fd, header, sizeof(header)) < (ssize_t)sizeof(header))
        return false;

    if (header[0] != 0x11)
        return false;

    id3->filesize = ffilesize(fd);

    /* the full header was read, so filesize >= CAS_DATA_OFFSET */
    program_length = (header[0x83] << 8) + header[0x82];
    if (program_length > id3->filesize - CAS_DATA_OFFSET)
        return false;

    id3->frequency = 44100;
    id3->bitrate = 706; /* 16-bit mono PCM at 44.1 kHz */
    id3->vbr = false;

    /* Length of the tape the codec generates, in samples at 44.1 kHz.
     * Only the bit periods depend on the data, so count each bit as the
     * average of a "0" (24 samples) and a "1" (17 samples).
     */
    samples = 2 * 44100                 /* silence */
            + 10240 * 21 + 32           /* leader, sync */
            + 32 * 41 * 8 / 2           /* head block bytes */
            + 5 * 21;                   /* trailer */
    if (program_length > 0) {
        unsigned long sectors = (program_length + 255) / 256;
        samples += 44100                /* silence */
                 + 5120 * 21 + 32       /* leader, sync */
                 /* block header, sector headers and CRCs, data */
                 + (6 + 5 * sectors + program_length) * 41 * 8 / 2
                 + 5 * 21;              /* trailer */
    }
    samples += 2 * 44100;               /* silence */

    id3->length = samples * 1000 / 44100;

    return true;
}
