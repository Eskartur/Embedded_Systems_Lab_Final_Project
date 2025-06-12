#define SAMPLE_RATE 22050
#define DETREND_N (SAMPLE_RATE/200-1)
#define DETREND_BUF_LEN (DETREND_N*2+1)
#define MAX_PERIOD (SAMPLE_RATE/80)
#define MIN_PERIOD (SAMPLE_RATE/500)
#define PSOLA_HALF_BUF_LEN (6*MAX_PERIOD)
#define PSOLA_BUF_LEN (2*PSOLA_HALF_BUF_LEN)
#define PSOLA_PEAKS_LEN (PSOLA_BUF_LEN/MIN_PERIOD+1)

#include "uart_printf.h"
#include "main.h"
extern UART_HandleTypeDef huart1;

/*variables for epoch extraction*/
static float prev_sample = 0; //HPF buffer with only one value

static const float r = 0.98f;
static const float zpzfr_coeffs[] = {4*r, -6*r*r, 4*r*r*r, -1*r*r*r*r};
static float zpzfr_buffer[] = {0, 0, 0, 0};

static float detrend_buffer[DETREND_BUF_LEN] = {0};
static float delay_buffer[DETREND_N] = {0};
static float moving_sum = 0;

static float prev_sample_detrended = 0;

static int16_t prev_peak_pos = 0;
static int16_t peaks_list[PSOLA_PEAKS_LEN] = {0};
static int16_t peaks_list_len = 0;

static int16_t zpzfr_pos = 0;
static int16_t detrend_pos = 0;
static int16_t delay_pos = 0;
/*------------------------------*/

/*variables for PSOLA*/
static float psola_in_buffer[PSOLA_BUF_LEN] = {0};
static float psola_out_buffer[PSOLA_BUF_LEN] = {0};
static int16_t audio_out[PSOLA_HALF_BUF_LEN] = {0};

static int16_t psola_pos = 0;

static float pitch_ratio = 1; //0.4719f;
static float formant_ratio = 1; //0.8409f;

static int16_t a_peak = 0; // current analysis peak
static int16_t a_peak_idx = 0; // current analysis peak index
static int16_t a_lw = 0; // left window width
static int16_t a_rw = 0; // right window width

static int16_t s_peak = PSOLA_HALF_BUF_LEN/2 + 1; // current synthesis peak
static int16_t s_lw = 0; // left window width
static int16_t s_rw = 0; // right window width

static float lw_signal[MAX_PERIOD*2+1] = {0};
static float rw_signal[MAX_PERIOD*2+1] = {0};
/*-------------------*/

void PSOLA_init(float pitch_factor, float formant_factor) {
	// reset variables
	/*
	prev_sample = 0;
	for (int16_t i = 1; i < 4; i++) {
		zpzfr_buffer[i] = 0;
	}
	for (int16_t i = 1; i < DETREND_BUF_LEN; i++) {
		detrend_buffer[i] = 0;
	}
	for (int16_t i = 1; i < DETREND_N; i++) {
		delay_buffer[i] = 0;
	}
	moving_sum = 0;
	prev_peak_pos = 0;
	for (int16_t i = 1; i < PSOLA_PEAKS_LEN; i++) {
		peaks_list[i] = 0;
	}
	peaks_list_len = 0;
	zpzfr_pos = 0;
	detrend_pos = 0;
	delay_pos = 0;
	for (int16_t i = 1; i < PSOLA_BUF_LEN; i++) {
		psola_in_buffer[i] = 0;
	}
	for (int16_t i = 1; i < PSOLA_BUF_LEN; i++) {
		psola_out_buffer[i] = 0;
	}
	for (int16_t i = 1; i < PSOLA_HALF_BUF_LEN; i++) {
		audio_out[i] = 0;
	}
	psola_pos = 0;
	*/
	pitch_ratio = pitch_factor;
	formant_ratio = formant_factor;
	/*
	a_peak = 0;
	a_peak_idx = 0;
	a_lw = 0;
	a_rw = 0;
	s_peak = PSOLA_HALF_BUF_LEN/2 + 1;
	s_lw = 0;
	s_rw = 0;
	for (int16_t i = 1; i < MAX_PERIOD*2+1; i++) {
		lw_signal[i] = 0;
	}
	for (int16_t i = 1; i < MAX_PERIOD*2+1; i++) {
		rw_signal[i] = 0;
	}
	*/
}

static uint8_t start_marker[4] = {0xDE, 0xAD, 0xBE, 0xEF};

void PSOLA_feed(int16_t feed_length, int16_t *feed) {

    for(int16_t feed_pos = 0; feed_pos < feed_length; feed_pos++){

    	//HPF
    	float sample = (float)feed[feed_pos];
    	float sample_hpf = sample - prev_sample;
    	prev_sample = sample;

    	//ZP-ZFR
    	float sample_zpzfr =
    			sample_hpf +
    			zpzfr_buffer[(zpzfr_pos-1+4)%4] * zpzfr_coeffs[0] +
				zpzfr_buffer[(zpzfr_pos-2+4)%4] * zpzfr_coeffs[1] +
				zpzfr_buffer[(zpzfr_pos-3+4)%4] * zpzfr_coeffs[2] +
				zpzfr_buffer[(zpzfr_pos-4+4)%4] * zpzfr_coeffs[3];

    	zpzfr_buffer[zpzfr_pos] = sample_zpzfr;
    	zpzfr_pos = (zpzfr_pos+1)%4;

    	//De-trending (subtracting local mean) with delay of DETREND_N samples
    	float sample_detrended = detrend_buffer[(detrend_pos-DETREND_N+DETREND_BUF_LEN)%DETREND_BUF_LEN] - moving_sum/DETREND_BUF_LEN;
    	moving_sum += sample_zpzfr - detrend_buffer[detrend_pos];
    	detrend_buffer[detrend_pos] = sample_zpzfr;
    	detrend_pos = (detrend_pos+1)%DETREND_BUF_LEN;

    	//get the original sample with delay of DETREND_N samples
    	float sample_delayed = delay_buffer[delay_pos];
    	delay_buffer[delay_pos] = sample;
    	delay_pos = (delay_pos+1)%DETREND_N;

    	//Zero-crossing
    	if ((prev_sample_detrended < 0 && sample_detrended >= 0 && psola_pos-prev_peak_pos >= MIN_PERIOD) || psola_pos-prev_peak_pos >= MAX_PERIOD) {
    		prev_peak_pos = psola_pos;
    		peaks_list[peaks_list_len] = psola_pos;
    		peaks_list_len++;
    	}
    	prev_sample_detrended = sample_detrended;

    	// PSOLA

    	psola_in_buffer[psola_pos] = sample_delayed;//copy the sample to in_buffer
    	psola_pos++;

    	if (psola_pos == PSOLA_BUF_LEN) {

    		//compute PSOLA
    		while (1) {
				s_peak += s_rw; // move peak

				s_lw = s_rw;

				// find the nearest peak
				while (a_peak_idx+1 < peaks_list_len &&  !(peaks_list[a_peak_idx] <= s_peak && s_peak < peaks_list[a_peak_idx+1])) {
					a_peak_idx++;
				}


				if (s_peak-peaks_list[a_peak_idx] < peaks_list[a_peak_idx+1]) { // select the peak to the left
					a_peak = peaks_list[a_peak_idx];
					s_rw = (int16_t)((peaks_list[a_peak_idx+1] - peaks_list[a_peak_idx]) / pitch_ratio); // set new right window width
				} else if (a_peak_idx+2 < peaks_list_len) { // select the peak to the right
					a_peak = peaks_list[a_peak_idx+1];
					s_rw = (int16_t)((peaks_list[a_peak_idx+2] - peaks_list[a_peak_idx+1]) / pitch_ratio); // set new right window width
				}

				a_lw = (int16_t)(s_lw * formant_ratio);
				a_rw = (int16_t)(s_rw * formant_ratio);

				// left window
				for (int16_t i = 0; i < a_lw+1; i++) {
					if (a_peak-i >= 0) {
						lw_signal[i] = psola_in_buffer[a_peak-i];
					} else {
						lw_signal[i] = 0;
					}
				}

				// interpolate
				int16_t interp = 1;
				for (int16_t i = 1; i < s_lw+1; i++) {
					float x = (float)i*a_lw/s_lw;
					while (interp < x) {
						interp++;
					}
					float y;
					if (interp > a_lw) {
						y = lw_signal[a_lw];
					} else {
						y = lw_signal[interp-1] * (interp-x) + lw_signal[interp] * (x-interp+1);
					}
					if (s_peak-i >= 0) {
						psola_out_buffer[s_peak-i] += (float)(s_lw-i)/s_lw * y;
					}
				}

				psola_out_buffer[s_peak] += psola_in_buffer[a_peak]; // peak

				// right window
				for (int16_t i = 0; i < a_rw+1; i++) {
					if (a_peak+i < PSOLA_BUF_LEN) {
						rw_signal[i] = psola_in_buffer[a_peak+i];
					} else {
						rw_signal[i] = 0;
					}
				}

				// interpolate
				interp = 1;
				for (int16_t i = 1; i < s_rw+1; i++) {
					float x = (float)i*a_rw/s_rw;
					while (interp < x) {
						interp++;
					}
					float y;
					if (interp > a_rw) {
						y = rw_signal[a_rw];
					} else {
						y = rw_signal[interp-1] * (interp-x) + rw_signal[interp] * (x-interp+1);
					}
					if (s_peak+i < PSOLA_BUF_LEN) {
						psola_out_buffer[s_peak+i] += (float)(s_rw-i)/s_lw * y;
					}
				}

				// end condition
				if (s_peak >= PSOLA_HALF_BUF_LEN + PSOLA_HALF_BUF_LEN/2 + 1) {
					break;
				}
    		}

    		// for the next loop
    		a_peak_idx = 0;
    		s_peak -= PSOLA_HALF_BUF_LEN;

    		//output audio and copy the second half to the first half
    		for (int16_t i = 0; i < PSOLA_HALF_BUF_LEN; i++) {
    			if (psola_out_buffer[i] <= 32767 && psola_out_buffer[i] >= -32768) {
    				audio_out[i] = (int16_t)psola_out_buffer[i];
    			} else if (psola_out_buffer[i] > 32767) {
    				audio_out[i] = 32767;
    			} else {
    				audio_out[i] = -32768;
    			}
    			psola_in_buffer[i] = psola_in_buffer[i+PSOLA_HALF_BUF_LEN];
    			psola_out_buffer[i] = psola_out_buffer[i+PSOLA_HALF_BUF_LEN];
    			psola_out_buffer[i+PSOLA_HALF_BUF_LEN] = 0;
    		}

    		//output to UART
    		HAL_UART_Transmit(&huart1, start_marker, 4, 1000);
    		HAL_UART_Transmit(&huart1, (uint8_t*)audio_out, PSOLA_HALF_BUF_LEN * 2, 1000);

    		//move the peak index array left PSOLA_HALF_BUF_LEN samples (discard negatives)
    		int16_t new_start_idx = -1;
    		for (int16_t idx = 0; idx < peaks_list_len; idx++) {
    			if (peaks_list[idx] >= PSOLA_HALF_BUF_LEN) {
    				if (new_start_idx == -1) {
    					new_start_idx = idx;
    				}
    				peaks_list[idx-new_start_idx] = peaks_list[idx] - PSOLA_HALF_BUF_LEN;
    			}
    		}
    		peaks_list_len -= new_start_idx;
    		prev_peak_pos -= PSOLA_HALF_BUF_LEN;

    		psola_pos = PSOLA_HALF_BUF_LEN; //continue from the middle
    	}
    }
}
