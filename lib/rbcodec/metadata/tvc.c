#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>

#include "system.h"
#include "metadata.h"
#include "metadata_common.h"
#include "metadata_parsers.h"
#include "rbunicode.h"

#define LOGF_ENABLE
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
bool get_tvc_metadata(int fd, struct mp3entry *id3)
{
    logf("Getting TVC metadata");

    id3->frequency = 44100;
    id3->bitrate = id3->frequency / 10;
    id3->vbr = false;
    id3->filesize = ffilesize(fd);

    /* Rough estimate of the length:
     * header: 128 + 16 bytes in file, about 5s + 2s silence on tape
     * data: average 470 usec per bit on tape
     */
    id3->length = 7000 + ((id3->filesize - 144) * 8 * 470LL) / 1000;

    return true;
}
