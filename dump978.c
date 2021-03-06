//
// Copyright 2015, Oliver Jowett <oliver@mutability.co.uk>
//

// This file is free software: you may copy, redistribute and/or modify it  
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 2 of the License, or (at your  
// option) any later version.  
//
// This file is distributed in the hope that it will be useful, but  
// WITHOUT ANY WARRANTY; without even the implied warranty of  
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU  
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License  
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "uat.h"
#include "fec/rs.h"

void *rs_uplink;
void *rs_adsb_short;
void *rs_adsb_long;

static void make_atan2_table();
static void read_from_stdin();
static int process_buffer(uint16_t *phi, int len, uint64_t offset);
static int demod_adsb_frame(uint16_t *phi, uint8_t *to, int *rs_errors);
static int demod_uplink_frame(uint16_t *phi, uint8_t *to, int *rs_errors);
static void demod_frame(uint16_t *phi, uint8_t *frame, int bytes, int16_t center_dphi);
static void handle_adsb_frame(uint64_t timestamp, uint8_t *frame, int rs);
static void handle_uplink_frame(uint64_t timestamp, uint8_t *frame, int rs);

#define UPLINK_POLY 0x187
#define ADSB_POLY 0x187

#define SYNC_BITS (36)
#define ADSB_SYNC_WORD   0xEACDDA4E2UL
#define UPLINK_SYNC_WORD 0x153225B1DUL

// relying on signed overflow is theoretically bad. Let's do it properly.

#ifdef USE_SIGNED_OVERFLOW
#define phi_difference(from,to) ((int16_t)((to) - (from)))
#else
inline int16_t phi_difference(uint16_t from, uint16_t to)
{
    int32_t difference = to - from; // lies in the range -65535 .. +65535
    if (difference >= 32768)        //   +32768..+65535
        return difference - 65536;  //   -> -32768..-1: always in range
    else if (difference < -32768)   //   -65535..-32769
        return difference + 65536;  //   -> +1..32767: always in range
    else
        return difference;
}
#endif

int main(int argc, char **argv)
{
    rs_adsb_short = init_rs_char(8, /* gfpoly */ ADSB_POLY, /* fcr */ 120, /* prim */ 1, /* nroots */ 12, /* pad */ 225);
    rs_adsb_long  = init_rs_char(8, /* gfpoly */ ADSB_POLY, /* fcr */ 120, /* prim */ 1, /* nroots */ 14, /* pad */ 207);
    rs_uplink     = init_rs_char(8, /* gfpoly */ UPLINK_POLY, /* fcr */ 120, /* prim */ 1, /* nroots */ 20, /* pad */ 163);

    make_atan2_table();
    read_from_stdin();
    return 0;
}

static void dump_raw_message(char updown, uint8_t *data, int len, int rs_errors)
{
    int i;

    fprintf(stdout, "%c", updown);
    for (i = 0; i < len; ++i) {
        fprintf(stdout, "%02x", data[i]);
    }

    if (rs_errors)
        fprintf(stdout, ";rs=%d", rs_errors);
    fprintf(stdout, ";\n");
}

static void handle_adsb_frame(uint64_t timestamp, uint8_t *frame, int rs)
{
    dump_raw_message('-', frame, (frame[0]>>3) == 0 ? SHORT_FRAME_DATA_BYTES : LONG_FRAME_DATA_BYTES, rs);
    fflush(stdout);
}

static void handle_uplink_frame(uint64_t timestamp, uint8_t *frame, int rs)
{
    dump_raw_message('+', frame, UPLINK_FRAME_DATA_BYTES, rs);
    fflush(stdout);
}

uint16_t iqphase[65536]; // contains value [0..65536) -> [0, 2*pi)

void make_atan2_table()
{
    unsigned i,q;
    union {
        uint8_t iq[2];
        uint16_t iq16;
    } u;

    for (i = 0; i < 256; ++i) {
        for (q = 0; q < 256; ++q) {
            double d_i = (i - 127.5);
            double d_q = (q - 127.5);
            double ang = atan2(d_q, d_i) + M_PI; // atan2 returns [-pi..pi], normalize to [0..2*pi]
            double scaled_ang = round(32768 * ang / M_PI);

            u.iq[0] = i;
            u.iq[1] = q;
            iqphase[u.iq16] = (scaled_ang < 0 ? 0 : scaled_ang > 65535 ? 65535 : (uint16_t)scaled_ang);
        }
    }
}

static void convert_to_phi(uint16_t *buffer, int n)
{
    int i;

    for (i = 0; i < n; ++i)
        buffer[i] = iqphase[buffer[i]];
}

void read_from_stdin()
{
    char buffer[65536*2];
    int n;
    int used = 0;
    uint64_t offset = 0;
    
    while ( (n = read(0, buffer+used, sizeof(buffer)-used)) > 0 ) {
        int processed;

        convert_to_phi((uint16_t*) (buffer+(used&~1)), ((used&1)+n)/2);

        used += n;
        processed = process_buffer((uint16_t*) buffer, used/2, offset);
        used -= processed * 2;
        offset += processed;
        if (used > 0) {
            memmove(buffer, buffer+processed*2, used);
        }
    }
}

// maximum number of bit errors to permit in the sync word
#define MAX_SYNC_ERRORS 2

// check that there is a valid sync word starting at 'phi'
// that matches the sync word 'pattern'. Place the dphi
// threshold to use for bit slicing in '*center'. Return 1
// if the sync word is OK, 0 on failure
int check_sync_word(uint16_t *phi, uint64_t pattern, int16_t *center)
{
    int i;
    int32_t dphi_zero_total = 0;
    int zero_bits = 0;
    int32_t dphi_one_total = 0;
    int one_bits = 0;
    int error_bits;

    // find mean dphi for zero and one bits;
    // take the mean of the two as our central value

    for (i = 0; i < 36; ++i) {
        int16_t dphi = phi_difference(phi[i*2], phi[i*2+1]);

        if (pattern & (1UL << (35-i))) {
            ++one_bits;
            dphi_one_total += dphi;
        } else {
            ++zero_bits;
            dphi_zero_total += dphi;
        }
    }

    dphi_zero_total /= zero_bits;
    dphi_one_total /= one_bits;

    *center = (dphi_one_total + dphi_zero_total) / 2;

    // recheck sync word using our center value
    error_bits = 0;
    for (i = 0; i < 36; ++i) {
        int16_t dphi = phi_difference(phi[i*2], phi[i*2+1]);

        if (pattern & (1UL << (35-i))) {
            if (dphi < *center)
                ++error_bits;
        } else {
            if (dphi >= *center)
                ++error_bits;
        }
    }

    //fprintf(stdout, "check_sync_word: center=%.0fkHz, errors=%d\n", *center * 2083334.0 / 65536 / 1000, error_bits);

    return (error_bits <= MAX_SYNC_ERRORS);
}

int process_buffer(uint16_t *phi, int len, uint64_t offset)    
{
    uint64_t sync0 = 0, sync1 = 0;
    int lenbits;
    int bit;

    uint8_t demod_buf_a[UPLINK_FRAME_BYTES];
    uint8_t demod_buf_b[UPLINK_FRAME_BYTES];

    // We expect samples at twice the UAT bitrate.
    // We look at phase difference between pairs of adjacent samples, i.e.
    //  sample 1 - sample 0   -> sync0
    //  sample 2 - sample 1   -> sync1
    //  sample 3 - sample 2   -> sync0
    //  sample 4 - sample 3   -> sync1
    // ...
    //
    // We accumulate bits into two buffers, sync0 and sync1.
    // Then we compare those buffers to the expected 36-bit sync word that
    // should be at the start of each UAT frame. When (if) we find it,
    // that tells us which sample to start decoding from.

    // Stop when we run out of remaining samples for a max-sized frame.
    // Arrange for our caller to pass the trailing data back to us next time;
    // ensure we don't consume any partial sync word we might be part-way
    // through. This means we don't need to maintain state between calls.

    // We actually only look for the first CHECK_BITS of the sync word here.
    // If there's a match, the frame demodulators will derive a center offset
    // from the full word and then use that to re-check the sync word. This
    // lets us grab a few more marginal frames.

    // 18 seems like a good tradeoff between recovering more frames
    // and excessive false positives
#define CHECK_BITS 18
#define CHECK_MASK ((1UL<<CHECK_BITS)-1)
#define CHECK_ADSB (ADSB_SYNC_WORD >> (SYNC_BITS-CHECK_BITS))
#define CHECK_UPLINK (UPLINK_SYNC_WORD >> (SYNC_BITS-CHECK_BITS))

    lenbits = len/2 - ((SYNC_BITS-CHECK_BITS) + UPLINK_FRAME_BITS);
    for (bit = 0; bit < lenbits; ++bit) {
        int16_t dphi0 = phi_difference(phi[bit*2], phi[bit*2+1]);
        int16_t dphi1 = phi_difference(phi[bit*2+1], phi[bit*2+2]);

        sync0 = ((sync0 << 1) | (dphi0 > 0 ? 1 : 0));
        sync1 = ((sync1 << 1) | (dphi1 > 0 ? 1 : 0));

        if (bit < CHECK_BITS)
            continue; // haven't fully populated sync0/1 yet

        // see if we have (the start of) a valid sync word
        // It would be nice to look at popcount(expected ^ sync) 
        // so we can tolerate some errors, but that turns out
        // to be very expensive to do on every sample

        // when we find a match, try to demodulate both with that match
        // and with the next position, and pick the one with fewer
        // errors.

        // check for downlink frames:

        if ((sync0 & CHECK_MASK) == CHECK_ADSB || (sync1 & CHECK_MASK) == CHECK_ADSB) {
            int startbit = (bit-CHECK_BITS+1);
            int shift = ((sync0 & CHECK_MASK) == CHECK_ADSB) ? 0 : 1;
            int index = startbit*2+shift;

            int skip_0, skip_1;
            int rs_0 = -1, rs_1 = -1;

            skip_0 = demod_adsb_frame(phi+index, demod_buf_a, &rs_0);
            skip_1 = demod_adsb_frame(phi+index+1, demod_buf_b, &rs_1);
            if (skip_0 && rs_0 <= rs_1) {
                handle_adsb_frame(offset+index, demod_buf_a, rs_0);
                bit = startbit + skip_0;
                continue;
            } else if (skip_1 && rs_1 <= rs_0) {
                handle_adsb_frame(offset+index+1, demod_buf_b, rs_1);
                bit = startbit + skip_1;
                continue;
            } else {
                // demod failed
            }
        }

        // check for uplink frames:

        else if ((sync0 & CHECK_MASK) == CHECK_UPLINK || (sync1 & CHECK_MASK) == CHECK_UPLINK) {
            int startbit = (bit-CHECK_BITS+1);
            int shift = ((sync0 & CHECK_MASK) == CHECK_UPLINK) ? 0 : 1;
            int index = startbit*2+shift;

            int skip_0, skip_1;
            int rs_0 = -1, rs_1 = -1;

            skip_0 = demod_uplink_frame(phi+index, demod_buf_a, &rs_0);
            skip_1 = demod_uplink_frame(phi+index+1, demod_buf_b, &rs_1);
            if (skip_0 && rs_0 <= rs_1) {
                handle_uplink_frame(offset+index, demod_buf_a, rs_0);
                bit = startbit + skip_0;
                continue;
            } else if (skip_1 && rs_1 <= rs_0) {
                handle_uplink_frame(offset+index+1, demod_buf_b, rs_1);
                bit = startbit + skip_1;
                continue;
            } else {
                // demod failed
            }
        }
    }

    return (bit - CHECK_BITS)*2;
}

// demodulate 'bytes' bytes from samples at 'phi' into 'frame',
// using 'center_dphi' as the bit slicing threshold
static void demod_frame(uint16_t *phi, uint8_t *frame, int bytes, int16_t center_dphi)
{
    while (--bytes >= 0) {
        uint8_t b = 0;
        if (phi_difference(phi[0], phi[1]) > center_dphi) b |= 0x80;
        if (phi_difference(phi[2], phi[3]) > center_dphi) b |= 0x40;
        if (phi_difference(phi[4], phi[5]) > center_dphi) b |= 0x20;
        if (phi_difference(phi[6], phi[7]) > center_dphi) b |= 0x10;
        if (phi_difference(phi[8], phi[9]) > center_dphi) b |= 0x08;
        if (phi_difference(phi[10], phi[11]) > center_dphi) b |= 0x04;
        if (phi_difference(phi[12], phi[13]) > center_dphi) b |= 0x02;
        if (phi_difference(phi[14], phi[15]) > center_dphi) b |= 0x01;
        *frame++ = b;
        phi += 16;
    }
}

// Demodulate an ADSB (Long UAT or Basic UAT) downlink frame
// with the first sync bit in 'phi', storing the frame into 'to'
// of length up to LONG_FRAME_BYTES. Set '*rs_errors' to the
// number of corrected errors, or 9999 if demodulation failed.
// Return 0 if demodulation failed, or the number of bits (not
// samples) consumed if demodulation was OK.
static int demod_adsb_frame(uint16_t *phi, uint8_t *to, int *rs_errors)
{
    int16_t center_dphi;
    int n_corrected;

    if (!check_sync_word(phi, ADSB_SYNC_WORD, &center_dphi)) {
        *rs_errors = 9999;
        return 0;
    }

    demod_frame(phi + SYNC_BITS*2, to, LONG_FRAME_BYTES, center_dphi);

    // Try decoding as a Long UAT.
    // We rely on decode_rs_char not modifying the data if there were
    // uncorrectable errors.
    n_corrected = decode_rs_char(rs_adsb_long, to, NULL, 0);
    if (n_corrected >= 0 && n_corrected <= 7 && (to[0]>>3) != 0) {
        // Valid long frame.
        *rs_errors = n_corrected;
        return (SYNC_BITS+LONG_FRAME_BITS);
    }

    // Retry as Basic UAT
    n_corrected = decode_rs_char(rs_adsb_short, to, NULL, 0);
    if (n_corrected >= 0 && n_corrected <= 6 && (to[0]>>3) == 0) {
        // Valid short frame
        *rs_errors = n_corrected;
        return (SYNC_BITS+SHORT_FRAME_BITS);
    }

    // Failed.
    *rs_errors = 9999;
    return 0;
}

// Demodulate an uplink frame
// with the first sync bit in 'phi', storing the frame into 'to'
// of length up to UPLINK_FRAME_BYTES. Set '*rs_errors' to the
// number of corrected errors, or 9999 if demodulation failed.
// Return 0 if demodulation failed, or the number of bits (not
// samples) consumed if demodulation was OK.
static int demod_uplink_frame(uint16_t *phi, uint8_t *to, int *rs_errors)
{
    int block;
    int16_t center_dphi;
    uint8_t interleaved[UPLINK_FRAME_BYTES];
    int total_corrected = 0;

    if (!check_sync_word(phi, UPLINK_SYNC_WORD, &center_dphi)) {
        *rs_errors = 9999;
        return 0;
    }

    demod_frame(phi + SYNC_BITS*2, interleaved, UPLINK_FRAME_BYTES, center_dphi);
    
    // deinterleave a block at a time directly into the target buffer
    // (we have enough space for the trailing ECC as the caller provides UPLINK_FRAME_BYTES)
    for (block = 0; block < UPLINK_FRAME_BLOCKS; ++block) {
        int i, n_corrected;
        uint8_t *blockdata = &to[block * UPLINK_BLOCK_DATA_BYTES];

        for (i = 0; i < UPLINK_BLOCK_BYTES; ++i)
            blockdata[i] = interleaved[i * UPLINK_FRAME_BLOCKS + block];

        // error-correct in place
        n_corrected = decode_rs_char(rs_uplink, blockdata, NULL, 0);
        if (n_corrected < 0 || n_corrected > 10) {
            *rs_errors = 9999;
            return 0;
        }

        total_corrected += n_corrected;
        // next block (if there is one) will overwrite the ECC bytes.
    }

    *rs_errors = total_corrected;
    return (UPLINK_FRAME_BITS+SYNC_BITS);
}
