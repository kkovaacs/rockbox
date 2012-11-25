#include "debug.h"
#include "codeclib.h"
#include <inttypes.h>

#define LOGF_ENABLE
#include "logf.h"

CODEC_HEADER

#define CHUNK_SIZE 1024
#define CAS_MAX_FILESIZE 65536
#define SAMPLE_RATE 44100

#define CAS_BLOCKSIZE 0x80

#define SILENCE 0x0000
#define POS_PEAK 0x7d00
#define NEG_PEAK -0x7d00

const int16_t zero_bit[24] ICONST_ATTR = {
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
};

const int16_t one_bit[16] ICONST_ATTR = {
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
};

const int16_t pre[18] ICONST_ATTR = {
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK,
};

const int16_t sync[34] ICONST_ATTR = {
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK, POS_PEAK, POS_PEAK, POS_PEAK,
    POS_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK, NEG_PEAK, NEG_PEAK, NEG_PEAK,
    NEG_PEAK,
};

#define DIM(array) (sizeof(array) / sizeof(array[0]))

static int16_t samples[CHUNK_SIZE] IBSS_ATTR;
static unsigned int samples_written = 0;
static uint16_t current_crc = 0;

static uint16_t crc(unsigned char bit, uint16_t crc)
{
    unsigned char a, carry;

    a = bit ? 0x80 : 0;
    a ^= crc >> 8;
    carry = a & 0x80;
    if (carry)
        crc ^= 0x0810;
    crc <<= 2;
    if (carry)
        crc++;

    return crc;
}

static void output(int16_t const *sample, unsigned int num)
{
    if (samples_written + num > CHUNK_SIZE) {
        ci->pcmbuf_insert(samples, NULL, samples_written);
        samples_written = 0;
    }

    memcpy(samples + samples_written, sample, num * sizeof(int16_t));
    samples_written += num;
}

static void out_silence_1s(void)
{
    unsigned int i = SAMPLE_RATE;

    memset(samples, 0, sizeof(samples));

    while (i > 0) {
        unsigned int len = MIN(i, DIM(samples));
        ci->pcmbuf_insert(samples, NULL, len);
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
    size_t length = fill_sample_buffer(pre, sizeof(pre));
    unsigned int num_in_buffer = length / sizeof(pre);

    while (num > 0) {
        unsigned int i = MIN(num, num_in_buffer);
        ci->pcmbuf_insert(samples, NULL, i * DIM(pre));
        num -= i;
    }
}

static void out_sync(void)
{
    output(sync, DIM(sync));
}

static void out_bit(unsigned char bit)
{
    if (bit == 0)
        output(zero_bit, DIM(zero_bit));
    else
        output(one_bit, DIM(one_bit));

    current_crc = crc(bit, current_crc);
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

static void out_string(unsigned char *str)
{
    unsigned int i;
    size_t len = strlen(str);

    if (len > 10) {
        DEBUGF("CODEC_ERROR: file name '%s' too long\n", str);
        return;
    }

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
    out_byte(1 + strlen("TEST") + 16); // bytes in sector: file name and non-buffered block header

    out_string("TEST"); // file name

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
    out_byte(len / 256 + 1); // sectors in this block
}

static void out_datablock(unsigned char *data, size_t len, unsigned int sector)
{
    if (sector > 1)
        current_crc = 0;

    out_byte(sector);
    out_byte(len & 0xffLL);

    for (; len > 0; len--, data++)
        out_byte(*data);

    out_word(current_crc);
}

static void out_data(unsigned char *casfile, size_t file_length)
{
    unsigned int i;
    size_t offset;
    size_t data_length = file_length - 0x90;

    out_datahead(data_length);

    for (i = 0, offset = 0; offset < data_length; i++, offset += 256) {
        size_t chunk_length = MIN(256, data_length - offset);
        out_datablock(casfile + offset, chunk_length, i + 1);
    }

    out_pre(5);
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
    size_t filesize;
    unsigned char *casfile = NULL;
    intptr_t param;
    uint16_t blocknumber;
    uint8_t lastblockbytes;
    size_t filesize_in_header;

    if (codec_init()) {
        return CODEC_ERROR;
    }

    codec_set_replaygain(ci->id3);

    ci->seek_buffer(0);
    casfile = ci->request_buffer(&filesize, CAS_MAX_FILESIZE);

    if (filesize == 0) {
        return CODEC_ERROR;
    }

    /* check UPM header */
    if (casfile[0] != 0x11) {
        DEBUGF("CODEC_ERROR: not a non-buffered CAS file: %u\n", (unsigned int)casfile[0]);
        return CODEC_ERROR;
    }

    blocknumber = (casfile[3] << 8) + casfile[2];
    lastblockbytes = casfile[4];
    filesize_in_header = blocknumber * CAS_BLOCKSIZE + lastblockbytes;

    DEBUGF("File size in header: %zu\n", filesize_in_header);
    DEBUGF("CAS type %u, program size %u, autorun %u, version %u\n",
           casfile[0x81], (casfile[0x83] << 8) + casfile[0x82],
           casfile[0x84], casfile[0x8f]);

    do {
        enum codec_command_action action = ci->get_command(&param);

        if (action == CODEC_ACTION_HALT)
            break;

        if (action == CODEC_ACTION_SEEK_TIME) {
            /* TODO: implement seeking */
            ci->set_elapsed(0);
            ci->seek_complete();
        }

        out_headblock(casfile);
        out_data(casfile, filesize_in_header);
    } while (0);

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
