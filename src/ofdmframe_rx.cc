/*
 * Copyright (c) 2011 Joseph Gaeddert
 * Copyright (c) 2011 Virginia Polytechnic Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <complex>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <liquid/liquid.h>

#include "usrp_io.h"
 
#define USRP_CHANNEL    (0)
#define DEBUG           (0)
#define DEBUG_FILENAME  "packetstream_rx_debug.m"
 
static bool verbose;

static int callback(std::complex<float> * _X,
                    unsigned int * _p,
                    unsigned int _M,
                    void * _userdata)
{
    printf("**** callback invoked\n");
    return 1;
}

void usage() {
    printf("ofdmframe_rx -- receive OFDM packets\n");
    printf("  f     :   center frequency [Hz]\n");
    printf("  b     :   bandwidth [Hz]\n");
    printf("  M     :   number of subcarriers, default: 64\n");
    printf("  t     :   run time [seconds]\n");
    printf("  q     :   quiet\n");
    printf("  v     :   verbose\n");
    printf("  u,h   :   usage/help\n");
}

int main (int argc, char **argv)
{
    // command-line options
    verbose = true;

    float min_bandwidth = (32e6 / 512.0);
    float max_bandwidth = (32e6 /   4.0);

    float frequency = 462.0e6;
    float bandwidth = min_bandwidth;
    float num_seconds = 5.0f;

    // 
    unsigned int M = 64;                // number of subcarriers
    unsigned int cp_len = 16;           // cyclic prefix length

    //
    int d;
    while ((d = getopt(argc,argv,"f:b:M:t:qvuh")) != EOF) {
        switch (d) {
        case 'f':   frequency = atof(optarg);       break;
        case 'b':   bandwidth = atof(optarg);       break;
        case 'M':   M = atoi(optarg);               break;
        case 't':   num_seconds = atof(optarg);     break;
        case 'q':   verbose = false;                break;
        case 'v':   verbose = true;                 break;
        case 'u':
        case 'h':
        default:
            usage();
            return 0;
        }
    }

    if (bandwidth > max_bandwidth) {
        fprintf(stderr,"error: %s, maximum symbol rate exceeded (%8.4f MHz)\n", argv[0], max_bandwidth*1e-6);
        exit(1);
    } else if (bandwidth < min_bandwidth) {
        fprintf(stderr,"error: %s, minimum symbol rate exceeded (%8.4f kHz)\n", argv[0], min_bandwidth*1e-3);
        exit(1);
    }

    printf("frequency   :   %12.8f [MHz]\n", frequency*1e-6f);
    printf("symbol rate :   %12.8f [kHz]\n", bandwidth*1e-3f);
    printf("verbosity   :   %s\n", (verbose?"enabled":"disabled"));

    unsigned int rx_buffer_length = 512;
    unsigned int num_blocks = (unsigned int)((2.0f*bandwidth*num_seconds)/(rx_buffer_length));

    // create usrp_io object and set properties
    usrp_io * uio = new usrp_io();
    uio->set_rx_freq(USRP_CHANNEL, frequency);
    uio->set_rx_samplerate(2.0f*2.0f*bandwidth);
    uio->enable_auto_tx(USRP_CHANNEL);

    // retrieve rx port
    gport port_rx = uio->get_rx_port(USRP_CHANNEL);

    // half-band decimator
    resamp2_crcf decim = resamp2_crcf_create(41,0.0f,40.0f);

    // initialize subcarrier allocation
    unsigned int p[M];
    ofdmframe_init_default_sctype(M, p);

    // create frame synchronizer
    ofdmframesync fs = ofdmframesync_create(M, cp_len, p, callback, NULL);
    ofdmframesync_print(fs);
 
    std::complex<float> data_rx[rx_buffer_length];

    // start data transfer
    uio->start_rx(USRP_CHANNEL);
    // consume first few blocks to allow hardware to settle
    gport_consume(port_rx,(void*)data_rx,rx_buffer_length);
    printf("usrp data transfer started\n");
 
    unsigned int n;
    unsigned int i;
    for (n=0; n<num_blocks; n++) {
        // grab data from port
        gport_consume(port_rx,(void*)data_rx,rx_buffer_length);

        for (i=0; i<rx_buffer_length; i+=2) {
            // push through half-band decimator
            std::complex<float>decim_out;
            resamp2_crcf_decim_execute(decim, &data_rx[i], &decim_out);

            // run through ofdm frame synchronizer
            ofdmframesync_execute(fs, &decim_out, 1);
        }
    }
 
 
    uio->stop_rx(USRP_CHANNEL);  // Stop data transfer
    printf("usrp data transfer complete\n");

    // destroy objects
    resamp2_crcf_destroy(decim);
    ofdmframesync_destroy(fs);

    delete uio;
    return 0;
}
