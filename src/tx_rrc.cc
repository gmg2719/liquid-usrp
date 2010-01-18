/*
 * Copyright (c) 2007, 2008, 2009, 2010 Joseph Gaeddert
 * Copyright (c) 2007, 2008, 2009, 2010 Virginia Polytechnic
 *                                      Institute & State University
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
 
#include <math.h>
#include <iostream>
#include <complex>
#include <getopt.h>
#include <liquid/liquid.h>

#include "usrp_io.h"
 
/*
 SAMPLES_PER_READ :Each sample is consists of 4 bytes (2 bytes for I and 
 2 bytes for Q. Since the reading length from USRP should be multiple of 512 
 bytes see "usrp_basic.h", then we have to read multiple of 128 samples each 
 time (4 bytes * 128 sample = 512 bytes)  
 */
#define SAMPLES_PER_READ    (512)       // Must be a multiple of 128
#define USRP_CHANNEL        (0)

void usage() {
    printf("packet_tx:\n");
    printf("  f     :   center frequency [Hz]\n");
    printf("  s     :   symbol rate [Hz] (62.5kHz min, 8MHz max)\n");
    printf("  t     :   run time [seconds]\n");
    printf("  m     :   filter delay [symbols]\n");
    printf("  b     :   filter excess bandwidth factor [0.0 min, 1.0 max]\n");
    printf("  q     :   quiet\n");
    printf("  v     :   verbose\n");
    printf("  u,h   :   usage/help\n");
}

int main (int argc, char **argv)
{
    // command-line options
    bool verbose = true;

    float min_symbol_rate = (32e6 / 512.0);
    float max_symbol_rate = (32e6 /   4.0);

    float frequency = 462.0e6;
    float symbol_rate = min_symbol_rate;
    float num_seconds = 5.0f;
    unsigned int m = 3;
    float beta = 0.3f;
    float r = 1.0f; // resampling rate

    //
    int d;
    while ((d = getopt(argc,argv,"f:s:t:m:b:qvuh")) != EOF) {
        switch (d) {
        case 'f':   frequency = atof(optarg);       break;
        case 's':   symbol_rate = atof(optarg);     break;
        case 't':   num_seconds = atof(optarg);     break;
        case 'm':   m = atoi(optarg);               break;
        case 'b':   beta = atof(optarg);            break;
        case 'q':   verbose = false;                break;
        case 'v':   verbose = true;                 break;
        case 'u':
        case 'h':
        default:
            usage();
            return 0;
        }
    }

    // compute interpolation rate
    unsigned int interp_rate = (unsigned int)(32e6 / symbol_rate);
    
    // ensure multiple of 4
    interp_rate = (interp_rate >> 2) << 2;

    if (symbol_rate > max_symbol_rate) {
        printf("error: maximum symbol_rate exceeded (%8.4f MHz)\n", max_symbol_rate*1e-6);
        return 0;
    } else if (symbol_rate < min_symbol_rate) {
        printf("error: minimum symbol_rate exceeded (%8.4f kHz)\n", min_symbol_rate*1e-3);
        return 0;
    } else if (m < 1 || m > 20) {
        printf("error: filter length m must be in [1,20]\n");
        return 0;
    } else if (beta < 0.0f || beta > 1.0f) {
        printf("error: filter excess bandwidth beta must be in [0.0,1.0]\n");
        return 0;
    }

    // compute usrp symbol rate
    float usrp_symbol_rate = 32e6f / (float)(interp_rate);

    printf("frequency   :   %12.8f [MHz]\n", frequency*1e-6f);
    printf("symbol_rate :   %12.8f [kHz] (usrp : %12.8f [kHz])\n", symbol_rate*1e-3f, usrp_symbol_rate*1e-3f);
    printf("verbosity   :   %s\n", (verbose?"enabled":"disabled"));

    //
    r = usrp_symbol_rate / symbol_rate;
    printf("resampling rate : %12.8f\n", r);

    // 
    unsigned int num_blocks = (unsigned int)((4.0f*symbol_rate*num_seconds)/(512));

    // create usrp_io object and set properties
    usrp_io * uio = new usrp_io();
    uio->set_tx_freq(0, frequency);
    uio->set_tx_interp(interp_rate);
    uio->enable_auto_tx(0);

    // retrieve tx port from usrp_io object
    gport2 port_tx = uio->get_tx_port(0);

    unsigned int num_symbols = 128;

    // filter parameters
    unsigned int k=2;   // samples/symbol
    //unsigned int m=3;   // symbol delay
    float dt=0.0f;      // fractional sample delay
    //float beta=0.3f;    // excess bandwidth factor

    // create filter
    unsigned int h_len = 2*k*m+1;
    float h[h_len];
    design_rrc_filter(k,m,beta,dt,h);
    interp_crcf nyquist_filter = interp_crcf_create(k,h,h_len);

    resamp2_crcf interpolator = resamp2_crcf_create(37,0.0f,60.0f);
    std::complex<float> data_tx[512];

    // arbitrary resampler
    resamp_crcf arbitrary_resampler = resamp_crcf_create(r,13,0.5f,60.0f,32);
    unsigned int num_written;
    unsigned int num_written_total;
    std::complex<float> data_resamp[1024];

    // 
    std::complex<float> symbols[num_symbols];
    std::complex<float> interp_out[2*num_symbols];

    // modem
    modulation_scheme ms = MOD_QPSK;
    unsigned int bps = 2;
    modem mod = modem_create(ms,bps);

    unsigned int n; // sample counter
    unsigned int s; // data symbol
    
    unsigned int i;

    // start USRP data transfer
    uio->start_tx(0);
    for (i=0; i<num_blocks; i++) {

        // generate data symbols
        for (n=0; n<num_symbols; n++) {
            s = modem_gen_rand_sym(mod);
            modem_modulate(mod,s,&symbols[n]);
        }

        // run nyquist filter/interpolator
        for (n=0; n<num_symbols; n++)
            interp_crcf_execute(nyquist_filter,symbols[n],&interp_out[2*n]);

        // run half-band interpolator
        for (n=0; n<2*num_symbols; n++)
            resamp2_crcf_interp_execute(interpolator,interp_out[n],&data_tx[2*n]);

        // run arbitrary resampler
        num_written_total = 0;
        for (n=0; n<4*num_symbols; n++) {
            resamp_crcf_execute(arbitrary_resampler,data_tx[n],&data_resamp[num_written_total],&num_written);
            num_written_total += num_written;
        }
        //printf(" %6u > %6u\n", 4*num_symbols, num_written_total);

        //gport2_produce(port_tx,(void*)data_tx,512);
        gport2_produce(port_tx,(void*)data_resamp,num_written_total);
    }
 
    uio->stop_tx(0);  // Stop data transfer
    printf("usrp data transfer complete\n");

    // clean it up
    resamp2_crcf_destroy(interpolator);
    resamp_crcf_destroy(arbitrary_resampler);
    interp_crcf_destroy(nyquist_filter);
    modem_destroy(mod);
    delete uio;
    return 0;
}

