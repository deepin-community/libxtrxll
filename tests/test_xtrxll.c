/*
 * xtrx low level general test file
 * Copyright (c) 2017 Sergey Kostanbaev <sergey.kostanbaev@fairwaves.co>
 * For more information, please visit: http://xtrx.io
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "xtrxll_port.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "xtrxll_api.h"
#include "xtrxll_mmcm.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <semaphore.h>
#include <string.h>
#include "xtrxll_log.h"

// for getopt
#include <unistd.h>


#define MAX_BUFFS   256

static volatile int g_pipe_broken = 0;
static volatile int g_exit_flag = 0;

void signal_pipe(int signo)
{
	g_pipe_broken = 1;
}

void signal_int(int signo)
{
	g_exit_flag = 1;
}

struct dma_buff {
	char  ptr[32768];
	size_t sz;
};

struct dma_buff g_out_buffs[MAX_BUFFS];
sem_t           g_out_buff_available;
int             g_out_stream;

void *write_thread(void* dev)
{
	uint32_t buf_rd = 0;

#ifdef __linux
	sigset_t set;
	sigfillset(&set);
	pthread_sigmask(SIG_SETMASK, &set, NULL);
#endif

	for (;;) {
		sem_wait(&g_out_buff_available);

		if (g_exit_flag)
			break;

		void* ptr = g_out_buffs[buf_rd & (MAX_BUFFS-1)].ptr;
		size_t sz = g_out_buffs[buf_rd & (MAX_BUFFS-1)].sz;

		ssize_t res = write(g_out_stream, ptr, sz);
		if (res < 0) {
			g_exit_flag = 1;
			break;
		}
		//xtrxll_dma_rx_release((struct xtrxll_dev *)dev, 0, ptr);

		buf_rd++;
	}

	return NULL;
}

struct dma_buff g_in_buffs[MAX_BUFFS];
sem_t           g_in_buff_available;
sem_t           g_in_buff_ready;
int             g_in_stream;

void *read_thread(void* dev)
{
	uint32_t buf_wr = 0;

#ifdef __linux
	sigset_t set;
	sigfillset(&set);
	pthread_sigmask(SIG_SETMASK, &set, NULL);
#endif

	for (;;) {
		sem_wait(&g_in_buff_available);

		if (g_exit_flag)
			break;

		void* ptr = g_in_buffs[buf_wr & (MAX_BUFFS-1)].ptr;
		size_t sz = g_in_buffs[buf_wr & (MAX_BUFFS-1)].sz;

		ssize_t res = read(g_in_stream, ptr, sz);
		if (res < 0) {
			g_exit_flag = 1;
			break;
		}

		buf_wr++;
		sem_post(&g_in_buff_ready);
	}

	return NULL;
}

const uint32_t LMS_VERSION = 0x002fffff;

static uint64_t grtime(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

	return ts.tv_sec * 1000000000 + ts.tv_nsec;
}



static int create_stream(const char* filename, int flags)
{
	int fd = open(filename, flags);
	if (fd < 0) {
		perror("Can't open file");
		exit(EXIT_FAILURE);
	}
	return fd;
}

static int create_out_stream(const char* filename)
{
	g_out_stream = create_stream(filename, O_WRONLY);
	return g_out_stream;
}

static int create_in_stream(const char* filename)
{
	g_in_stream = create_stream(filename, O_RDONLY);
	return g_in_stream;
}

void do_calibrate_tcxo(struct xtrxll_dev *dev, int range, int whole, int step)
{
	int j, osc, val, res;
	xtrxll_set_param(dev, XTRXLL_PARAM_REF_DAC, 0);
	sleep(1);
	xtrxll_set_param(dev, XTRXLL_PARAM_REF_DAC, 0);
	sleep(1);

	printf("dac,1pps captured,osc latched,temperature\n");
	for (int i = range; i < whole-range; i+=step) {
		res = xtrxll_get_sensor(dev, XTRXLL_ONEPPS_CAPTURED, &j);
		res = xtrxll_get_sensor(dev, XTRXLL_ONEPPS_CAPTURED, &j);
		//if (res == 0) {
		res = xtrxll_get_sensor(dev, XTRXLL_OSC_LATCHED, &osc);
		//}

		if (res) {
			printf("\nABORTED!\n");
			return;
		}

		//xtrxll_set_osc_dac(dev, 0x3000);
		xtrxll_set_param(dev, XTRXLL_PARAM_REF_DAC, i);
		usleep(100); /*
		for (k = 0; k < 32; k++) {
			xtrxll_set_osc_dac(dev, i + 1);
			usleep(1000);
		}
		*/
		xtrxll_get_sensor(dev, XTRXLL_TEMP_SENSOR_CUR, &val);
		printf("%d,%d,%d,%.3f\n", i, j, osc, val/256.0);
	}

}

int do_tcxo_calibration(struct xtrxll_dev *dev, int* ferr, double fref, int dac_granularity)
{
	//const int D[2] = { 1024 + 256, 3072 - 256 };
	const int D[2] = { (0 + 1024)/dac_granularity, (65535 - 1024)/dac_granularity };
	int res;
	int j, i;
	int osc[3];
	int osc_q[3];

	res = xtrxll_get_sensor(dev, XTRXLL_ONEPPS_CAPTURED, &j);

	for (i = 0; i < 2; i++) {
		xtrxll_set_param(dev, XTRXLL_PARAM_REF_DAC, D[i]*dac_granularity);
		usleep(1000);
		xtrxll_set_param(dev, XTRXLL_PARAM_REF_DAC, D[i]*dac_granularity);

		res = xtrxll_get_sensor(dev, XTRXLL_ONEPPS_CAPTURED, &j);
		res = xtrxll_get_sensor(dev, XTRXLL_ONEPPS_CAPTURED, &j);
		if (res == 0) {
			res = xtrxll_get_sensor(dev, XTRXLL_OSC_LATCHED, &osc[i]);
		}

		res = xtrxll_get_sensor(dev, XTRXLL_ONEPPS_CAPTURED, &j);
		res = xtrxll_get_sensor(dev, XTRXLL_ONEPPS_CAPTURED, &j);
		if (res == 0) {
			res = xtrxll_get_sensor(dev, XTRXLL_OSC_LATCHED, &osc_q[i]);
		}

		printf("%d (%d raw) => %d %d\n", D[i]*dac_granularity, D[i], osc[i], osc_q[i]);
	}

	double k = ((double)(osc[1] + osc_q[1] - osc[0] - osc_q[0])) / (D[1] - D[0]);
	double Q = D[0] + (2 * fref - osc[0] - osc_q[0]) / k;
	int Q_int = (int)(Q + 0.5);

	printf("k=%.3f Q=%d\n", k / 2, Q_int);

	xtrxll_set_param(dev, XTRXLL_PARAM_REF_DAC, Q_int*dac_granularity);
	usleep(1000);
	xtrxll_set_param(dev, XTRXLL_PARAM_REF_DAC, Q_int*dac_granularity);
	res = xtrxll_get_sensor(dev, XTRXLL_ONEPPS_CAPTURED, &j);
	res = xtrxll_get_sensor(dev, XTRXLL_ONEPPS_CAPTURED, &j);
	if (res == 0) {
		res = xtrxll_get_sensor(dev, XTRXLL_OSC_LATCHED, &osc[i]);
	}

	printf("Linear => %d\n", osc[2]);

	*ferr = (fref - osc[2]);
	return Q_int*dac_granularity;
}

void do_test_1pps(struct xtrxll_dev *dev, int initial_dac, double fref)
{
	int ini = -1;
	int err = -1;
	const int DAC_GRANULARITY = 16; // Minimum DAC step

	if (initial_dac < 0) {
		ini = do_tcxo_calibration(dev, &err, fref, DAC_GRANULARITY);
		ini = ini/DAC_GRANULARITY;
	}


	int res;
	int i = -1;
	int osc = 0;
	int last_upd = 0;


	const int FILTER_BITS = 3;

	const int32_t ORIG_FREQ = fref;
	const int32_t MAX_ERR   = ORIG_FREQ / 10000; // 100ppm

	uint64_t freq_c = 0; // Averaged frequency

	const int     DAC_RANGE = 65536/DAC_GRANULARITY;
	const int     DAC_WORK_RANGE = 2800/DAC_GRANULARITY;
	const int64_t VCTXCO_PULL_RANGE = ORIG_FREQ * 36 / 1000000; //36 ppm pullability range

	int ctrl_prev = 0;
	int ctrl = 0;
	int skip_upd = 0;
	int initial = 1;

	const int DAC_CHANGE_THRES = 25;


	double e2 = 0, e1 = 0, e0 = 0, u2 = 0, u1 = 0, u0 = 0;
	const double Kp = 0.5 * DAC_WORK_RANGE / VCTXCO_PULL_RANGE;   // proportional gain
	const double Ki = 1e-1; 	// integral gain
	const double Kd = 1e-3;  	// derivative gain
	const int N = 100;   		// filter coefficients
	const int Ts = 1;

	const double a0 = (1+N*Ts);
	const double a1 = -(2 + N*Ts);
	const double a2 = 1;
	const double b0 = Kp*(1+N*Ts) + Ki*Ts*(1+N*Ts) + Kd*N;
	const double b1 = -(Kp*(2+N*Ts) + Ki*Ts + 2*Kd*N);
	const double b2 = Kp + Kd*N;
	const double ku1 = a1/a0;
	const double ku2 = a2/a0;
	const double ke0 = b0/a0;
	const double ke1 = b1/a0;
	const double ke2 = b2/a0;

	const double meas_ppb = 100;

	if (ini >= 0 && ini < DAC_RANGE) {
		printf("Bootstrapping to %d (%d raw), freq error %d Hz\n", ini*DAC_GRANULARITY, ini, err);
		e0 = e1 = e2 = err;
		u0 = u1 = u2 = ini - (DAC_RANGE / 2);

	} else {
		initial_dac = initial_dac/DAC_GRANULARITY;
		if (initial_dac < 0 || initial_dac >= DAC_RANGE) {
			initial_dac = DAC_RANGE / 2;
		}
		u0 = u1 = u2 = initial_dac - (DAC_RANGE / 2);
		xtrxll_set_param(dev, XTRXLL_PARAM_REF_DAC, initial_dac*DAC_GRANULARITY);
		sleep(1);
		xtrxll_set_param(dev, XTRXLL_PARAM_REF_DAC, initial_dac*DAC_GRANULARITY);
	}

	int settled = 0;
	time_t start = time(NULL);

	for (;;) {
		res = xtrxll_get_sensor(dev, XTRXLL_ONEPPS_CAPTURED, &i);
		if (res == 0) {
			res = xtrxll_get_sensor(dev, XTRXLL_OSC_LATCHED, &osc);
		}

		last_upd += (i > 0) ? i : 1;

		int delta = ORIG_FREQ  - osc;
		printf("1PPS: %d: %d (%+d)\n", i, osc, delta);

		if (delta < -MAX_ERR || delta > MAX_ERR)
			continue;
		if (skip_upd) {
			skip_upd = 0;
			continue;
		}

		// Average frequency to get sub-Hz precision but only if the frequency
		// is no more than 2 Hz from the average.
		int fdc = (freq_c >> FILTER_BITS) - osc;
		if (freq_c == 0 || fdc < -2 || fdc > 2) {
			freq_c = ((uint64_t)osc) << FILTER_BITS;
		} else {
			freq_c = freq_c - (freq_c >> FILTER_BITS) + osc;
		}

		printf("   Freq: %.3f Hz\n", (double)freq_c / (1<<FILTER_BITS));
		int64_t precise_delta = ((uint64_t)ORIG_FREQ << FILTER_BITS) - freq_c;

		//int64_t D = (DAC_RANGE * precise_delta / VCTXCO_PULL_RANGE);
		//ctrl = -(D >> FILTER_BITS)/3  + ctrl_prev / 20;
		//printf("   D: %d (%ld %ld)\n", ctrl, precise_delta, D >> FILTER_BITS);

		double e0_t = (double)precise_delta / (1<<FILTER_BITS);
		double u0_t = -ku1*u1 - ku2*u2 + ke0*e0 + ke1*e1 + ke2*e2;
		e2=e1; e1=e0; e0=e0_t; u2=u1; u1=u0; u0=u0_t;
		printf(" e0=%f e1=%f e2=%f   u0=%f u1=%f u2=%f\n", e0, e1, e2, u0, u1, u2);

		time_t now = time(NULL);
		if ((fabs(e0) < ((double)ORIG_FREQ * meas_ppb / 1e+9)) &&
		    (fabs(e1) < ((double)ORIG_FREQ * meas_ppb / 1e+9)) &&
		    (fabs(e2) < ((double)ORIG_FREQ * meas_ppb / 1e+9)) ) {
			if (!settled)
				start = now;
			settled = 1;
		} else {
			if (settled)
				start = now;
			settled = 0;
		}
		printf("GPSDO status: %s for %.1f ppb precision for %d sec (current precision %.1f ppb)\n",
		       settled?"Settled":"Not settled", meas_ppb, (int)(now - start), fabs(e0)/(double)ORIG_FREQ*1e+9);

		ctrl = u0 + 0.5;

		// Clamp control
		if (ctrl < -(DAC_RANGE / 2) )
			ctrl = -(DAC_RANGE / 2);
		else if (ctrl > DAC_RANGE / 2 - 1)
			ctrl = DAC_RANGE / 2 - 1;

		if (initial || (ctrl_prev - ctrl) < -DAC_CHANGE_THRES || (ctrl_prev - ctrl) > DAC_CHANGE_THRES) {
			freq_c = 0;
			initial = 0;
		}

		// Only update DAC value if it's actually changed
		if (ctrl_prev != ctrl) {
			// We're going to change the frequency now, so we should skip one pps
			// to avoid bogus frequency calculation
			skip_upd = 1;

			uint32_t dac_value = ((DAC_RANGE / 2) + ctrl) * DAC_GRANULARITY;
			printf("  DC: %d DAC: %d\n", ctrl, dac_value);
			xtrxll_set_param(dev, XTRXLL_PARAM_REF_DAC, dac_value);

			// Validate that we've written a correct value, else alarm and exit
			uint32_t dac_value_real = 0xDEADBEEF;
			res = xtrxll_get_sensor(dev, XTRXLL_DAC_REG, (int*)&dac_value_real);
			if (dac_value_real != dac_value) {
				printf("ERROR: Real DAC %d != requested DAC value %d\n", dac_value_real, dac_value);
				break;
			}
		}
		ctrl_prev = ctrl;
	}
}

void do_ledtest(struct xtrxll_dev *dev, int testno)
{
	int res;

	//res = xtrxll_set_param(dev, XTRXLL_PARAM_GPIO_FUNC, (1 << 8) | (1 << 10) | (1 << 12));
	res = xtrxll_set_param(dev, XTRXLL_PARAM_GPIO_FUNC, 0);
	if (res)
		return;

	res = xtrxll_set_param(dev, XTRXLL_PARAM_GPIO_DIR, 7 << 4);
	if (res)
		return;

	for (unsigned i = 0; i < 64; i++) {
		res = xtrxll_set_param(dev, XTRXLL_PARAM_GPIO_OUT, i << 4);
		if (res)
			return;

		usleep(250000);
	}
}

void do_synctest(struct xtrxll_dev *dev, bool internal)
{
	int res;
	res = xtrxll_set_param(dev, XTRXLL_PARAM_GTIME_RESET, 1);
	if (res)
		return;
	res = xtrxll_set_param(dev, XTRXLL_PARAM_GPIO_FUNC, (1 << 0) | (1 << 2) | (1 << 22));
	if (res)
		return;
	res = xtrxll_set_param(dev, XTRXLL_PARAM_GPIO_DIR, 7 << 4);
	if (res)
		return;


	res = xtrxll_set_param(dev, XTRXLL_PARAM_PPSDO_CTRL, XTRXLL_PPSDO_DISABLE);
	if (res)
		return;
	res = xtrxll_set_param(dev, XTRXLL_PARAM_GTIME_CTRL, XTRXLL_GTIME_DISABLE);
	if (res)
		return;
	res = xtrxll_set_param(dev, XTRXLL_PARAM_ISOPPS_CTRL, XTRXLL_GISO_DISABLE);
	if (res)
		return;

	res = xtrxll_set_param(dev, XTRXLL_PARAM_GTIME_RESET, 0);
	if (res)
		return;

	struct xtrxll_gtime_cmd cc;
	cc.type = XTRXLL_GCMDT_GPIO_SET;
	cc.cmd_idx = 0;
	cc.param = 7 << 4;
	res = xtrxll_set_param(dev, XTRXLL_PARAM_GTIME_LOAD_CMD, (uintptr_t)&cc);
	if (res)
		return;

	cc.type = XTRXLL_GCMDT_GPIO_SET;
	cc.cmd_idx = 1;
	cc.param = 0;
	res = xtrxll_set_param(dev, XTRXLL_PARAM_GTIME_LOAD_CMD, (uintptr_t)&cc);
	if (res)
		return;

	struct xtrxll_gtime_time dd;
	for (unsigned i = 0; i < 16; i++) {
		dd.d_idx = 0;
		dd.d_cnt = 1;
		dd.frac = 0;
		dd.sec = 4100 + 2 * i;
		res = xtrxll_set_param(dev, XTRXLL_PARAM_GTIME_LOAD_TIME, (uintptr_t)&dd);
		if (res)
			return;

		dd.d_idx = 1;
		dd.d_cnt = 1;
		dd.frac = 0;
		dd.sec = 4100 + 2 * i + 1;
		res = xtrxll_set_param(dev, XTRXLL_PARAM_GTIME_LOAD_TIME, (uintptr_t)&dd);
		if (res)
			return;
	}

	res = xtrxll_set_param(dev, XTRXLL_PARAM_PPSDO_CTRL, XTRXLL_PPSDO_INT_GPS);
	if (res)
		return;

	res = xtrxll_set_param(dev, XTRXLL_PARAM_GTIME_SETCMP, 25999972);
	if (res)
		return;

	res = xtrxll_set_param(dev, XTRXLL_PARAM_GTIME_CTRL,
						   internal ? XTRXLL_GTIME_INT_ISO : XTRXLL_GTIME_EXT_PPSFW);
	if (res)
		return;

	res = xtrxll_set_param(dev, XTRXLL_PARAM_ISOPPS_CTRL, XTRXLL_GISO_PPSFW);
	if (res)
		return;


	res = xtrxll_set_param(dev, XTRXLL_PARAM_ISOPPS_SETTIME, 4096);
	if (res)
		return;


	for (unsigned i = 0; i < 640000; i++) {
		int a[2],c,d;

		res = xtrxll_get_sensor(dev, XTRXLL_GTIME_SECFRAC, &a[0]);
		if (res)
			return;
		res = xtrxll_get_sensor(dev, XTRXLL_GTIME_OFF, &c);
		if (res)
			return;
		res = xtrxll_get_sensor(dev, XTRXLL_OSC_LATCHED, &d);

		printf("%06d.%08d => %09d  [%02d] %c   %08d\n", a[0],a[1],c & 0x7fffff,
			   (unsigned)c >> 28, ((c >> 27)& 1) ? 'Y' : 'N', d);
		sleep(1);

	}
}

#if 0
int octo_lo_spi(struct xtrxll_dev *dev, uint32_t out)
{
	int res;
	res = xtrxll_set_param(dev, XTRXLL_PARAM_EXT_SPI,
						   0x10000000 | (out & 0x0fffffff));
	if (res)
		return res;

	res = xtrxll_set_param(dev, XTRXLL_PARAM_EXT_SPI,
						   0x20000000 | (out >> 28));
	if (res)
		return res;

	usleep(150000);
	return 0;
}

int octo_tune(struct xtrxll_dev* dev)
{
	uint32_t tregs[] = {
		0x1041C,
		0x61300B,
		0xC00C3A,
		0x8083CC9,
		0x102D0428,
		0x120000E7,
		0x3500A3F6,
		0x800025,
		0x30008B84,
		0x3,
		0x80032,
		0x4AAAAA1,
		0x200B60,

		0x00C00C3A,
		0x3500A3F6,
		0x30008B84 | 0x10,
		0x00080032,
		0x04AAAAA1,
		0x00000B60,
		0x30008B84,

		0x200B60
	};

	for (unsigned i = 0; i < sizeof(tregs)/sizeof(tregs[0]); i++) {
		fprintf(stderr, "=== %08x\n", tregs[i]);
		int res = octo_lo_spi(dev, tregs[i]);
		if (res) {
			return res;
		}
		usleep(5000);
	}
	return 0;
}
#endif


void do_octotest(struct xtrxll_dev *dev)
{
	int res;
	int oval;

	// Set
	// gpio_spi_sck, gpio_spi_sen, gpio_spi_mosi,

	res = xtrxll_set_param(dev, XTRXLL_PARAM_GPIO_FUNC, (1 << 20) | (0 << 18) | (1 << 16));
	if (res)
		return;

	res = xtrxll_set_param(dev, XTRXLL_PARAM_GPIO_DIR, 0);
	if (res)
		return;

	res = xtrxll_set_param(dev, XTRXLL_PARAM_EXT_SPI, 0x80000000);
	if (res)
		return;

	usleep(20000);

	res = xtrxll_get_sensor(dev, XTRXLL_EXT_SPI_RB, &oval);
	if (res)
		return;

	printf("GOT: %08x\n", oval);


	res = xtrxll_set_param(dev, XTRXLL_PARAM_EXT_SPI, 0x000001ff);
	if (res)
		return;

	//octo_tune(dev);
	sleep(100);

	printf("SHUTDOWN\n");

	res = xtrxll_set_param(dev, XTRXLL_PARAM_EXT_SPI, 0x00000000);
	if (res)
		return;
}

void usage(char* cmdname) {
	printf("Usage:\n"
	       " %s [-D device] [-P] [-T tempsensor] [-R] [-r fefmt] [-a dac_val] [-o]\n"
	       "\n"
	       "Command line options:\n"
	       "  -1                Run GPSDO algorithm (also see option -Z). If dac_val is set with option -a, it's used as the start value, otherwise the start value is calculated.\n"
	       "  -Z                Reference clock frequency for the GPSDO algorithm (see option -1) [default=26000000]"
	       "  -2                Calibrate TCXO DAC by iterating over all DAC values from dac_start to 65535-dac_start (see option -C) with step 4. Takes 2 sec per step.\n"
	       "  -C dac_start      Start value for the TCXO DAC calibration (see option -2) [default=0]",
	        cmdname);
}

int main(int argc, char** argv)
{
	struct xtrxll_dev *dev;
	uint32_t result;
	int num_lms7;
	uint32_t i;
	int opt;
	const char* device = NULL;
	int powerdown = 0;
	int temp_sensor = -1;
	int rxdma = -1;
	int txdma = -1;
	int do_reset = -1;
	//int do_osc = -1;
	int set_dac = -1;
	int set_mmcm = -1;
	//int verbose = 0;
	int out_stream = -1;
	int in_stream = -1;
	int rx_ant = -1;
	int tx_ant = -1;
	int repeat_mode = -1;
	int stop_tx = 0;
	int test_1pps = 0;
	int cal_tcxo = 0;
	double fref = 26000000; //30720000;
	int crange = 0;
	int lf = 0;
	int uart = -1;
	int refclk_cntr = 0;
	int pmic_reg = -1;
	int discovery = 0;
	int mmcm_tx = 1;
	int vio = -1;
	int ledtest = 0;
	int synctest = 0;
	int octotest = 0;

	pthread_t out_thread, in_thread;
#ifdef __linux
	signal(SIGPIPE, signal_pipe);
#endif
	//	signal(SIGINT,  signal_int);
	sem_init(&g_out_buff_available, 0, 0);

	sem_init(&g_in_buff_available, 0, 0);
	sem_init(&g_in_buff_ready, 0, 0);

	while ((opt = getopt(argc, argv, "EYdF:fU:C:Z:21A:a:oD:PRT:r:m:vO:I:l:p:SV:Lh")) != -1) {
		switch (opt) {
		case 'E':
			octotest = 1;
			break;
		case 'Y':
			synctest = 1;
			break;
		case 'L':
			ledtest = 1;
			break;
		case 'd':
			discovery = 1;
			break;
		case 'F':
			pmic_reg = atoi(optarg);
			break;
		case 'f':
			refclk_cntr = 1;
			break;
		case 'U':
			uart = atoi(optarg);
			break;
		case 'C':
			crange = atoi(optarg);
			break;
		case 'Z':
			fref = atof(optarg);
			break;
		case '1':
			test_1pps = 1;
			break;
		case '2':
			cal_tcxo = 1;
			break;
		case 'S':
			stop_tx = 1;
			break;
		case 'p':
			repeat_mode = atoi(optarg);
			break;
		case 'l':
			xtrxll_set_loglevel(atoi(optarg));
			break;
		case 'A':
			rx_ant = atoi(optarg) % 4;
			tx_ant = (atoi(optarg) >> 2) & 1;
			break;
		case 'v':
			//verbose = 1;
			break;
		case 'D':
			device = optarg;
			break;
		case 'P':
			powerdown = 1;
			break;
		case 'T':
			temp_sensor = atoi(optarg);
			break;
		case 'R':
			do_reset = 1;
			break;
		case 'r':
			rxdma = atoi(optarg);
			break;
		case 't':
			txdma = atoi(optarg);
			break;
		case 'a':
			set_dac = atoi(optarg);
			break;
		case 'o':
			lf = 1;
			break;
		case 'm':
			set_mmcm = atoi(optarg);
			break;
		case 'O':
			out_stream = create_out_stream(optarg);
			break;
		case 'I':
			in_stream  = create_in_stream(optarg);
			break;
		case 'V':
			vio = atoi(optarg);
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default: /* '?' */
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (discovery) {
		xtrxll_device_info_t buff[32];
		int count = xtrxll_discovery(buff, 32);
		for (int i = 0 ; i < count; i++) {
			printf("%d: %s %s %d %d\n", i, buff[i].uniqname, buff[i].addr, buff[i].product_id, buff[i].revision);
		}
		return 0;
	}

	int res = xtrxll_open(device, 0, &dev);
	if (res)
		goto falied_open;

	if (do_reset != -1 || powerdown) {
		res = xtrxll_set_param(dev, XTRXLL_PARAM_PWR_CTRL, PWR_CTRL_PDOWN);
		if (res || powerdown)
			goto falied_reset;

		usleep(10000);

		res = xtrxll_set_param(dev, XTRXLL_PARAM_PWR_CTRL, PWR_CTRL_ON);
		if (res)
			goto falied_reset;
		res = xtrxll_set_param(dev, XTRXLL_PARAM_FE_CTRL, 0);
		if (res)
			goto falied_reset;
		res = xtrxll_set_param(dev, XTRXLL_PARAM_FE_CTRL, 0xff);
		if (res)
			goto falied_reset;
	}

	res = xtrxll_get_sensor(dev, XTRXLL_CFG_NUM_RFIC, &num_lms7);
	if (res)
		goto falied_reset;

	for (i = 0; i < num_lms7; ++i) {
		res = xtrxll_lms7_spi_bulk(dev, XTRXLL_LMS7_0 << i, &LMS_VERSION, &result, 1);
		if (!res) {
			printf("Detected LMS #%d: %08x\n", i, result);
		}
	}

	if (vio > 100) {
		res = xtrxll_set_param(dev, XTRXLL_PARAM_PWR_VIO, vio);
		if (res)
			goto falied_reset;

		usleep(10000);
	}

	if (synctest) {
		do_synctest(dev, synctest == 1);
	}

	if (octotest) {
		do_octotest(dev);
	}

	if (lf) {
		int osc;
		res = xtrxll_get_sensor(dev, XTRXLL_OSC_LATCHED, &osc);
		if (!res) {
			printf("OSC FREQ is: %08x %d\n", osc, osc);
		}
	}

	if (temp_sensor >= 0) {
		if (temp_sensor > 2)
			temp_sensor = 2;
		int val;

		usleep(50000);
		res = xtrxll_get_sensor(dev, XTRXLL_TEMP_SENSOR_CUR + temp_sensor, &val);
		if (!res) {
			printf("Temp [%d]: %0.2fC (%04x)\n",
				   temp_sensor, (double)val / 256.0, val);
		}
	}

	if (pmic_reg != -1) {
		if (pmic_reg > 5)
			pmic_reg = 5;

		int val;
		res = xtrxll_get_sensor(dev, XTRXLL_PMIC0_VER + pmic_reg, &val);
		if (!res) {
			printf("PMIC: %08x\n", val);
		}
	}

	if (ledtest) {
		do_ledtest(dev, ledtest);
	}

	if (refclk_cntr) {
		int osc;
		int h;
		int prev = -1;
		int cur;

		for (h = 0; h < 32; h++) {
			res = xtrxll_get_sensor(dev, XTRXLL_REFCLK_CNTR, &osc);
			cur = (osc >> 16) & 0xff;
			if (cur == prev) {
				usleep(300);
				continue;
			}
			prev = cur;
			if (!res) {
				printf("REFCLK is: %2.3f MHZ (raw: %5d cnt: %3d / %3d)\n", (osc & 0xffff) * 62.5e6 / 32768 / 1e6, (osc & 0xffff), cur, (unsigned)osc >> 24);
			}
		}

		res = xtrxll_get_sensor(dev, XTRXLL_REFCLK_CLK, &osc);
		printf("REFCLK is: %d HZ\n", osc);
	}

	if (uart >= 0) {
		unsigned wr;
		char buffer[33] = {0,};
		for(;;) {
			res = xtrxll_read_uart(dev, uart, (uint8_t*)buffer, 32, &wr);
			if (res == -EAGAIN) {
				usleep(1000);
				continue;
			}

			buffer[wr] = 0;
			fputs(buffer, stderr);
		}
	}

	if (set_dac != -1) {
		res = xtrxll_set_param(dev, XTRXLL_PARAM_REF_DAC, set_dac);

		uint32_t out = 0xDEADBEEF;
		res = xtrxll_get_sensor(dev, XTRXLL_DAC_REG, (int*)&out);
		printf("DAC reg is: 0x%08x (%d)\n", out, out);
	}

	if (cal_tcxo) {
		do_calibrate_tcxo(dev, crange, 65535, 4);
	}

	if (test_1pps) {
		do_test_1pps(dev, set_dac, fref);
	}

	if (rx_ant != -1) {
		res = xtrxll_set_param(dev, XTRXLL_PARAM_SWITCH_RX_ANT, rx_ant);
	}
	if (tx_ant != -1) {
		res = xtrxll_set_param(dev, XTRXLL_PARAM_SWITCH_TX_ANT, tx_ant);
	}

	/*
	if (do_osc != -1) {
		uint32_t v;
		res = xtrxll_get_osc_freq(dev, &v);
		if (!res) {
			printf("Freq %.3f Hz (%08x)\n", (double)v / 32.0, v);
		}
	}
*/


	signal(SIGINT,  signal_int);

	if (stop_tx) {
		res = xtrxll_dma_tx_start(dev, 0, XTRXLL_FE_STOP, XTRXLL_FE_MODE_MIMO);
		if (res) {
			fprintf(stderr, "Unable to stop_tx err=%d\n", res);
			goto falied_reset;
		}
	}

	if (rxdma != -1) {
		if (out_stream != -1) {
			pthread_create(&out_thread, NULL, write_thread, dev);
		}

		int i;
		unsigned restarts = 0;
		unsigned bufsz;
		uint32_t wr_idx = 0;
		uint64_t start, delta;
		res = xtrxll_dma_rx_init(dev, 0, 0, &bufsz);
		if (res)
			goto falied_reset;

		start = grtime();
		res = xtrxll_dma_rx_start(dev, 0, rxdma);
		if (res)
			goto falied_reset;

		void* ptr;
		unsigned sz;
		wts_long_t wts;
		int j;
		for (j = 0, i = 0; !g_pipe_broken && !g_exit_flag; ++j) {
			res = xtrxll_dma_rx_getnext(dev, 0, &ptr, &wts, &sz, XTRXLL_RX_DONTWAIT, 0);
			if (res == 0) {
				if (out_stream != -1) {
					//size_t bsz = (rxdma == 3) ? sz :
					//							(rxdma == 2) ? (3*sz / 2) : sz / 2;
					//write(out_stream, ptr, sz);
					//if (rxdma == 3) {
					//	memcpybe(g_buffs[wr_idx & (MAX_BUFFS-1)].ptr, ptr, bsz);
					//} else {
					memcpy(g_out_buffs[wr_idx & (MAX_BUFFS-1)].ptr, ptr, sz);
					//}
					g_out_buffs[wr_idx & (MAX_BUFFS-1)].sz = sz;

					sem_post(&g_out_buff_available);
					wr_idx++;
				}
				xtrxll_dma_rx_release(dev, 0, ptr);

				++i;
				continue;
			} else if (res == -EOVERFLOW || res == -EPIPE) {
				//break;
				xtrxll_dma_rx_start(dev, 0, rxdma);
				usleep(10000);
				xtrxll_dma_rx_start(dev, 0, 0);

				usleep(1000);

				xtrxll_dma_rx_start(dev, 0, rxdma);

				restarts++;
			}
			usleep(500);
		}
		xtrxll_dma_rx_start(dev, 0, XTRXLL_FE_STOP);
		delta = grtime() - start;

		printf("Packets %d took %.6f sec -- %.3f MB/s  (res=%d restarts=%d)\n", i,
			   delta / 1000000000.0, ((32768.0 * 1000000000.0 * i / delta) / (1024*1024)),
			   res, restarts);

		res = xtrxll_dma_rx_deinit(dev, 0);
	}
	
	if (set_mmcm != -1) {
		res = xtrxll_mmcm_onoff(dev, mmcm_tx, set_mmcm != 0);
		if (res)
			goto falied_reset;

		if (set_mmcm != 0) {
			res = xtrxll_mmcm_setfreq(dev, mmcm_tx, set_mmcm, 0, 0, NULL, 0);
		}

		if (!res) {
			printf("MMCM was set\n");
		}
	}

	if (txdma != -1) {
		if (in_stream != -1) {
			pthread_create(&in_thread, NULL, read_thread, dev);
		}

		uint64_t start, delta;
		uint32_t wr_idx;
		int i, j;
		void* addr;
		size_t packetsz = (txdma == XTRXLL_FE_8BIT) ? 16384 :
													  (txdma == XTRXLL_FE_12BIT) ? 24576 : 32768;

		res = xtrxll_dma_tx_init(dev, 0, 0);
		if (res)
			goto falied_reset;

		start = grtime();
		res = xtrxll_dma_tx_start(dev, 0, txdma, XTRXLL_FE_MODE_MIMO);
		if (res)
			goto falied_reset;

		for (i = 0; i < MAX_BUFFS; ++i) {
			g_in_buffs[i].sz = packetsz;
			sem_post(&g_in_buff_available);
		}

		for (j = 0, i = 0; !g_pipe_broken && !g_exit_flag; ++j) {
			res = xtrxll_dma_tx_getfree_ex(dev, 0, &addr, NULL, 1000);
			if (res == -EBUSY) {
				break;
			} else if (res < 0) {
				break;
			} else if (res == 0) {
			}

			if (in_stream != -1) {
				res = sem_wait(&g_in_buff_ready);
				if (!res)
					break;
				memcpy(addr, g_in_buffs[wr_idx & (MAX_BUFFS-1)].ptr, packetsz);
				sem_post(&g_in_buff_available);

				wr_idx++;
			}

			res = xtrxll_dma_tx_post(dev, 0, addr, 0, 4096); // 4096 samples == 32768 bytes in 16bit IQ MIMO
			if (!res) {
				break;
			}
		}
		xtrxll_dma_tx_start(dev, 0, XTRXLL_FE_STOP, XTRXLL_FE_MODE_MIMO);
		delta = grtime() - start;

		printf("Packets %d took %.6f sec -- %.3f MB/s  (res=%d)\n", i,
			   delta / 1000000000.0, ((packetsz * 1000000000.0 * i / delta) / (1024*1024)),
			   res);

		res = xtrxll_dma_tx_deinit(dev, 0);
	}

	if (repeat_mode > 0) {
		if (repeat_mode > 4096)
			repeat_mode = 4096;
		uint16_t* mem = (uint16_t*)malloc(repeat_mode*2);
		for (unsigned i; i < repeat_mode; i++) {
			mem[i] = i;
		}

		res = xtrxll_dma_tx_start(dev, 0, XTRXLL_FE_STOP, XTRXLL_FE_MODE_MIMO);
		if (res) {
			fprintf(stderr, "Unable to stop tx err=%d\n", res);
			goto falied_reset;
		}

		res = xtrxll_repeat_tx_buf(dev, 0, XTRXLL_FE_16BIT, mem, repeat_mode*2, XTRXLL_FE_MODE_MIMO);
		if (res) {
			fprintf(stderr, "Unable to set repeat mode err=%d\n", res);
			goto falied_reset;
		}

		res = xtrxll_repeat_tx_start(dev, 0, 1);
		if (res) {
			fprintf(stderr, "Unable to start in repeat mode err=%d\n", res);
			goto falied_reset;
		}

	}

	if (out_stream != -1) {
		g_exit_flag = 1;
		sem_post(&g_out_buff_available);
		pthread_join(out_thread, NULL);
	}

	if (in_stream != -1) {
		g_exit_flag = 1;
		sem_post(&g_in_buff_available);
		pthread_join(in_thread, NULL);
	}

	return 0;
falied_reset:
	xtrxll_close(dev);
falied_open:
	return res;
}
