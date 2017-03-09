// A quick-n-dirty proof-of-concept fast C++ implementation.

// requires librtlsdr from https://github.com/rtlsdrblog/rtl-sdr
#define LIBRTLSDR_BIAS_TEE_SUPPORT

#include <algorithm>
#include <complex>
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <memory>

#include <cmath>

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdio.h>

#include <argp.h>

// fastcard and fastdet headers
#include <fastdet/corr_detector.h>
#include <fastdet/fastcard_wrappers.h>
#include <fastcard/parse.h>
#include <fastcard/rtlsdr_reader.h>

#include "sine_lookup.h"

using namespace std;

#define _USE_MATH_DEFINES
#define PI M_PI

#define MAX_TRACKING_ANGLE_DIFF 50
#define TRACKING_ANGLE_DIFF_FACTOR 0.2
#define AVG_ANGLE_WEIGHT 0.1
#define BEACON_INTERVAL_SEC 1

// time in seconds to capture after the first beacon detection
#define MAX_CAPTURE_TIME 10.1

// number of seconds after MAX_CAPTURE_TIME to capture data with the preamp
// switched off.
#define PREAMP_OFF_TIME 2.0

// amount of data to skip (in seconds) after the preamp is switced off.
#define PREAMP_OFF_SKIP 0.2

// #define OUTPUT_WINDOW_START 750
// #define OUTPUT_WINDOW_LEN 200

#define OUTPUT_WINDOW_START 0
#define OUTPUT_WINDOW_LEN -1

#define BEACON_CARRIER_TRIGGER_FACTOR 0.8
#define AVG_DCAMPL_WEIGHT 0.1


// Angles are stored as a value between -0.5 and 0.5 to simplify normalisation
// TODO: convert to class
using DeciAngle = float;
DeciAngle normalize_deciangle(DeciAngle angle) {
    return angle - int(round(angle));
}


void freq_shift_slow(complex<float> *dest,
                const complex<float> *src,
                size_t len,
                float shift_freq,
                DeciAngle shift_phase) {
    for (size_t i = 0; i < len; ++i) {
        dest[i] = exp(complex<float>(0, 2) * (float)PI * (shift_freq * i / (float)len + shift_phase)) * src[i];
        // dest[i] = SineLookup::expj(2 * (float)PI * (shift_freq * i / (float)len + shift_phase)) * src[i];
    }
}


// Apply a frequency and phase shift to the given signal.
// src and dest may be the same for inline transformation.
void freq_shift(complex<float> *dest,
                const complex<float> *src,
                size_t len,
                float shift_freq,
                DeciAngle shift_phase) {
    SineLookupNCO nco(2 * (float)PI * shift_phase,
                      2 * (float)PI * shift_freq / (float)len);
    nco.expj_multiply(dest, src, len);
}


// Like freq_shift, but accounts for discontinuity at DC due to FFT
// representation (i.e. zero-frequency at index 0).
void fft_shift(complex<float> *dest,
                const complex<float> *src,
                size_t len,
                float shift_freq,
                DeciAngle shift_phase,
                size_t carrier_offset) {
    // freq_shift(dest, src, len, shift_freq, shift_phase);
    // complex<float> phase_fix = SineLookup::expj(-2 * (float)PI * shift_freq);
    // for (size_t i = (len+1)/2 + carrier_offset; i < len; ++i) {
    //     dest[i] *= phase_fix;
    // }

    // Alternative:
    SineLookupNCO nco(2 * (float)PI * shift_phase,
                      2 * (float)PI * shift_freq / (float)len);
    size_t pos_len = (len+1)/2 + carrier_offset;  // number of positive frequency components
    nco.expj_multiply(dest, src, pos_len);
    nco.adjust_phase(-2 * (float)PI * shift_freq);
    nco.expj_multiply(dest+pos_len, src+pos_len, len-pos_len);
}


complex<float> calculate_dc(complex<float> *signal, size_t len) {
    std::complex<float> sum = std::accumulate(signal,
                                              signal + len,
                                              std::complex<float>(0, 0));
    return sum;  // / (float)len;
    // TODO: compare to fft(signal)
}


// // Calculate the complex argument at 0 Hz from a time-domain signal
// DeciAngle calculate_dc_phase(complex<float> *signal, size_t len) {
//     std::complex<float> sum = std::accumulate(signal,
//                                               signal + len,
//                                               std::complex<float>(0, 0));
// 
//     // calculate spectral density at DC
//     complex<float> dc = sum / (float)len;
//     return normalize_deciangle(arg(dc) / (float)PI / 2);
// }


inline complex<float>* to_complex_star(fcomplex* array) {
    return reinterpret_cast<complex<float>*>(array);
}


struct CorxFileHeader {
    uint16_t slice_start_idx;
    uint16_t slice_size;  // a.k.a. corr block length
} __attribute__((packed));


struct CorxBeaconHeader {
    double soa;
    uint64_t timestamp_sec;
    uint16_t timestamp_msec;
    uint32_t beacon_amplitude;
    uint32_t beacon_noise;
    float clock_error;
    float carrier_pos;
    uint32_t carrier_amplitude;
    bool preamp_on;
} __attribute__((packed));


// Assumptions:
//  - structs are byte-aligned
//  - ints are little endian
//  - IEEE floating points
// TODO: better portability (e.g. do not memcpy structs)
class CorxFileWriter {
public:
    CorxFileWriter(CFile&& out)
        : out_(std::move(out)), slice_size_(0) {};

    void write_file_header(const CorxFileHeader &header) {
        if (is_void()) {
            return;
        }

        // output file signature
        fprintf(out_.file(), "CORX");

        // output file format version
        fputc(version, out_.file());

        // output file header
        fwrite(reinterpret_cast<const char*>(&header),
               sizeof(header),
               1,
               out_.file());

        slice_size_ = header.slice_size;
    }

    void write_cycle_start(const CorxBeaconHeader &header) {
        // sein sterkte: carrier, beacon
        if (is_void()) {
            return;
        }

        fwrite(reinterpret_cast<const char*>(&header),
               sizeof(header),
               1,
               out_.file());
    }

    void write_cycle_block(int8_t phase_error,
                           const complex<float> *data,
                           uint16_t len) {
        if (is_void()) {
            return;
        }

        assert(len == slice_size_);
        assert(phase_error != -128);
        write_cycle_block_internal(phase_error, data, len);
    }

    void write_cycle_stop() {
        if (is_void()) {
            return;
        }

        // indicate end of cycle
        write_cycle_block_internal(-128, NULL, 0);
    }

    bool is_void() {
        return out_.file() == nullptr;
    }

private:
    void write_cycle_block_internal(int8_t phase_error,
                                    const complex<float> *data,
                                    uint16_t len) {
        fputc(phase_error, out_.file());

        fwrite(data,
               sizeof(complex<float>),
               len,
               out_.file());
    }

    CFile out_;
    int slice_size_;
    const static uint8_t version = 0x01;
};


class ArrayDetector {
public:
    // Warning: args should outlive ArrayDetector
    //  (TODO: use unique_ptr and move ownership of args)
    ArrayDetector(fargs_t* args,
                  std::string template_file,
                  float corr_thresh_const,
                  float corr_thresh_snr,
                  int corr_size,
                  CFile&& out)
                : args_(args),
                  synced_fft_calc_(args->block_len, true),
                  corr_size_(corr_size),
                  corr_fft_calc_(corr_size, true),
                  corrected_corr_fft_(corr_size),
                  writer_(std::move(out)) {

        carrier_det_.reset(new CarrierDetector(args_));
        vector<float> template_samples = load_template(template_file);
        corr_det_.reset(new CorrDetector(template_samples,
                                         args_->block_len,
                                         args_->history_len,
                                         corr_thresh_const,
                                         corr_thresh_snr));

        synced_signal_ = to_complex_star(synced_fft_calc_.input());
        synced_fft_ = to_complex_star(synced_fft_calc_.output());

        corr_signal_ = to_complex_star(corr_fft_calc_.input());
        corr_fft_ = to_complex_star(corr_fft_calc_.output());

        blocks_skip_ = args_->skip;
        
        num_cycles_ = ((args_->sdr_sample_rate - 2*skip_beacon_padding_) / 
                       corr_size_);

        slice_start_ = max(0, OUTPUT_WINDOW_START);
        slice_len_ = (OUTPUT_WINDOW_LEN <= 0) ? corr_size_
                     : min(corr_size_, (size_t)OUTPUT_WINDOW_LEN);

    }

    bool set_bias_tee(bool on) {
//#ifdef HAS_RTLSDR_BIAS
        if (strcmp(args_->input_file, "rtlsdr") != 0) {
            return false;
        }

#ifdef LIBRTLSDR_BIAS_TEE_SUPPORT
        rtlsdr_reader_set_bias_tee(carrier_det_->get()->reader, on);
        printf(on ? "Enabled bias tee\n" : "Disabled bias tee\n");
        return true;
#else
        return false;
#endif
    }

    void start() {
        detected_carrier_ = false;
        carrier_det_->start();

        set_bias_tee(true);

        writer_.write_file_header({(uint16_t)slice_start_,
                                   (uint16_t)slice_len_});
    }

    bool next() {
        if (preamp_off_block_ > 0 && block_idx_ == preamp_off_block_) {
            printf("block #%d: Switching off preamp...\n", block_idx_);

            if (cycle_ >= 0) {
                cycle_ = -1;
                writer_.write_cycle_stop();
            }

            set_bias_tee(false);

            blocks_skip_ = (PREAMP_OFF_SKIP * args_->sdr_sample_rate /
                            (args_->block_len - args_->history_len));
            printf("Skipping %d blocks...\n", blocks_skip_);
        }

        if (last_block_ > 0 && block_idx_ == last_block_) {
            carrier_det_->cancel();
        }

        // read the next block without performing carrier detection
        bool success = carrier_det_->next();
        if (!success) {
            if (cycle_ >= 0) {
                writer_.write_cycle_stop();
            }
            carrier_det_->print_stats(stdout);
            return false;
        }

        block_idx_++;

        if (blocks_skip_) {
            --blocks_skip_;
            return true;
        }

        if (preamp_off_block_ > 0 && block_idx_ > preamp_off_block_) {
            // continue with last carrier frequency from when the preamp was on
            freq_shift(synced_signal_,
                       to_complex_star(carrier_det_->data().samples),
                       args_->block_len,
                       -carrier_pos_,
                       sample_phase_);

            if (cycle_ == -1) {
                // FIXME: copy-pasta
                printf("block #%d: Capture noise: next cycle\n", block_idx_);

                soa_ = ((args_->block_len - args_->history_len) *
                        block_idx_);  // FIXME: no padding

                cycle_ = 0;
                num_phase_errors_ = 0;

                const struct timeval ts = carrier_det_->data().block->timestamp;
                CorxBeaconHeader header;
                header.soa = soa_;
                header.timestamp_sec = ts.tv_sec;
                header.timestamp_msec = ts.tv_usec / 1000;
                header.beacon_amplitude = 0;
                header.beacon_noise = 0;
                header.clock_error = clock_error_;  // N/A
                header.carrier_pos = carrier_pos_;
                header.carrier_amplitude = 0;
                header.preamp_on = false;
                writer_.write_cycle_start(header);
            }

            extract_corr_blocks();

            return true;
        }

        // calculate detected_carrier_, synced_signal_ and dc_angle_.
        recover_carrier();

        sample_phase_ -= carrier_pos_ * (1.f - (float)args_->history_len /
                                                      args_->block_len);
        sample_phase_ = normalize_deciangle(sample_phase_);

        avg_dc_angle_ = (dc_angle_ * AVG_ANGLE_WEIGHT +
                         avg_dc_angle_ * (1-AVG_ANGLE_WEIGHT));
        avg_dc_ampl_ = (dc_ampl_ * AVG_DCAMPL_WEIGHT +
                         avg_dc_ampl_ * (1-AVG_DCAMPL_WEIGHT));

        if (!detected_carrier_) {
            return true;
        }

        if (cycle_ == -1 && dc_ampl_ < avg_dc_ampl_ * BEACON_CARRIER_TRIGGER_FACTOR) {
            printf("DC: %.1f; avg: %.1f\n", dc_ampl_, avg_dc_ampl_);

            // TODO: use change in signal strength to determine whether it is
            // worth looking for a beacon signal

            CorrDetection corr = find_beacon();
            if (corr.detected) {
                clock_error_ = estimate_clock_error();

                printf("beacon #%d: ppm=%.3f\n",
                       beacon_,
                       clock_error_ * 1e6);

                // TODO: Asoa.append([pulse,soa,CarrierPosition,ppm]);
                cycle_ = 0;
                num_phase_errors_ = 0;

                if (beacon_ == 0) {
                    last_block_ = (((MAX_CAPTURE_TIME + PREAMP_OFF_TIME) *
                                    args_->sdr_sample_rate) /
                                   (args_->block_len - args_->history_len)
                                   + block_idx_);
                    printf("block %u: Found first beacon.\n"
                           "We'll stop after %.1f seconds "
                           "(at block block #%u).\n",
                           block_idx_,
                           (double)MAX_CAPTURE_TIME + PREAMP_OFF_TIME,
                           last_block_);

                    preamp_off_block_ = ((MAX_CAPTURE_TIME
                                          * args_->sdr_sample_rate)
                                          / (args_->block_len - args_->history_len)
                                         + block_idx_);
                }

                // TODO: move code below to extract_corr_blocks?
                const struct timeval ts = carrier_det_->data().block->timestamp;

                CorxBeaconHeader header;
                header.soa = soa_;
                header.timestamp_sec = ts.tv_sec;
                header.timestamp_msec = ts.tv_usec / 1000;
                header.beacon_amplitude = sqrt(corr.peak_power);
                header.beacon_noise = sqrt(corr.noise_power);  // FIXME
                header.clock_error = clock_error_;
                header.carrier_pos = carrier_pos_;
                header.carrier_amplitude = dc_ampl_;
                header.preamp_on = true;
                writer_.write_cycle_start(header);
            }

        }
        
        if (cycle_ >= 0) {
            extract_corr_blocks();  // pass parameters, e.g. clock_error_
        }

        return true;
    }

    void cancel() {
        if (carrier_det_) {
            carrier_det_->cancel();
        }
    }

protected:
    // Synchronise to / track the carrier.
    // Sets detected_carrier_, synced_signal_ and dc_angle_.
    // Returns false if carrier detection fails.
    bool recover_carrier() {
        if (detected_carrier_) {
            //// Carrier tracking and synchronization
            freq_shift(synced_signal_,
                       to_complex_star(carrier_det_->data().samples),
                       args_->block_len,
                       -carrier_pos_,
                       sample_phase_);

            prev_dc_angle_ = dc_angle_;

            complex<float> dc = calculate_dc(synced_signal_, args_->block_len);
            dc_ampl_ = abs(dc);
            dc_angle_ = normalize_deciangle(arg(dc) / (float)PI / 2);

            float angle_diff = normalize_deciangle(dc_angle_ - prev_dc_angle_);

            // printf("block #%u: Carrier phase error: %.0f deg\n",
            //        block_idx_,
            //        angle_diff * 360);

            if (angle_diff * 360 > MAX_TRACKING_ANGLE_DIFF) {
                // tracking loop failed
                detected_carrier_ = false;
                printf("block #%u: Tracking loop failed\n", block_idx_);
            } else {
                // track
                carrier_pos_ += angle_diff * TRACKING_ANGLE_DIFF_FACTOR;
            }
        }

        if (!detected_carrier_) {
            //// Tracking loop failed
            //// Carrier detection and synchronization

            carrier_det_->process();
            const fastcard_data_t& carrier = carrier_det_->data();

            if (carrier.detected) {
                float carrier_offset = CorrDetector::interpolate_parabolic(
                        &carrier.fft_power[carrier.detection.argmax]);
                carrier_pos_ = carrier.detection.argmax + carrier_offset;

                // calculate signed index
                if (carrier_pos_ > args_->block_len / 2) {
                    carrier_pos_ -= args_->block_len;
                }
                
                printf("block #%u: Detected carrier @ %.3f; SNR: %.1f / %.1f\n",
                       block_idx_,
                       carrier_pos_,
                       carrier.detection.max,
                       carrier.detection.noise);

                detected_carrier_ = true;

                // perform freq shift
                freq_shift(synced_signal_,
                           to_complex_star(carrier_det_->data().samples),
                           args_->block_len,
                           -carrier_pos_,
                           sample_phase_);

                complex<float> dc = calculate_dc(synced_signal_, args_->block_len);
                dc_ampl_ = abs(dc);
                dc_angle_ = normalize_deciangle(arg(dc) / (float)PI / 2);

            } else {
                printf("block #%u: No carrier detected\n", block_idx_);
            }
        }

        return detected_carrier_;
    }

    CorrDetection find_beacon() {
        synced_fft_calc_.execute();
        float signal_energy = 0; // TODO: calculate signal_energy
        CorrDetection corr = corr_det_->detect(synced_fft_, signal_energy);

        if (corr.detected) {
            printf("block #%d: detected beacon (ampl: %.0f)\n",
                   block_idx_,
                   corr.peak_power);
            
            prev_soa_ = soa_;
            soa_ = ((args_->block_len - args_->history_len) *
                     block_idx_ + corr.peak_idx) + corr.peak_offset;
            float time_step = (soa_ - prev_soa_) / args_->sdr_sample_rate;

            if ((beacon_ > 0) && (time_step > 1.5 * BEACON_INTERVAL_SEC)) {
                // We missed a pulse. Estimate beacon index from sample index.
                printf("Large time step!\n");
                beacon_ += (int)round(time_step);
            } else {
                beacon_++;
            }

            printf("beacon #%d: soa = %.3f; timestep = %.1f\n",
                   beacon_,
                   soa_,
                   time_step);
        }

        return corr;
    }

    void extract_corr_blocks() {
        // if (cycle_ == 0) {
        //     // write beacon header
        // }

        for (; cycle_ < num_cycles_; ++cycle_) {
            // calculate index of first sample in correlation block
            double start = (soa_
                            + (skip_beacon_padding_ + cycle_ * corr_size_)
                             * (1 - clock_error_)
                           - block_idx_
                             * (args_->block_len - args_->history_len));
            size_t start_idx = int(round(start));

            if (start_idx + corr_size_ > args_->block_len) {
                break;
            }

            memcpy(corr_signal_,
                   synced_signal_+start_idx,
                   corr_size_ * sizeof(complex<float>));

            // calculate FFT
            corr_fft_calc_.execute();

            // correct for complex phase offset and time offset
            fft_shift(corrected_corr_fft_.data(),
                      corr_fft_,
                      corr_size_,
                      start - start_idx,
                      -avg_dc_angle_,
                      -carrier_pos_ * corr_size_ / args_->block_len);

            DeciAngle error = arg(corrected_corr_fft_.data()[0]) / 2 / PI;
            if (abs(error) > 0.2) {
                num_phase_errors_++;
                // printf("Phase error > 0.2: %f\n", error);
            }


            // printf("block #%d, beacon #%d, cycle #%d, start %lu: error %.1f deg\n",
            //        block_idx_,
            //        beacon_,
            //        cycle_,
            //        start_idx,
            //        error / 2 / PI * 360);

            // Dump to output file
            int8_t error_fp = error / 0.5 * 127;
            writer_.write_cycle_block(error_fp,
                                      corrected_corr_fft_.data()+slice_start_,
                                      slice_len_);
        }

        if (cycle_ >= num_cycles_) {
            cycle_ = -1;
            writer_.write_cycle_stop();
            if (num_phase_errors_ > 0) {
                printf("beacon %d: %d / %d corr blocks have large phase error\n",
                       beacon_, num_phase_errors_, num_cycles_);
            }
        }
    }

    // Estimate the receiver's clock offset from the position of the carrier
    // frequency. It is assumed that the downconverter and ADC have the same
    // local oscillator (i.e. that they are coherent).
    float estimate_clock_error() {
        return (carrier_pos_ * args_->sdr_sample_rate / args_->block_len
                - carrier_ref_) / args_->sdr_freq;
    }

private:
    fargs_t* args_;

    // Read input and perform carrier detection using fastcard.
    std::unique_ptr<CarrierDetector> carrier_det_;

    // Perform correlation detection using fastdet.
    std::unique_ptr<CorrDetector> corr_det_;

    // Number of blocks read.
    unsigned block_idx_ = 0;

    // Number of blocks to skip.
    unsigned blocks_skip_;

    // Stop at the given block index (if > 0).
    unsigned last_block_ = 0;

    // Block index at which preamp should be switched off
    unsigned preamp_off_block_ = 0;

    // Phase of first sample in block.
    // Used to ensure a continuous phase between subsequent blocks.
    DeciAngle sample_phase_ = 0;

    // Position of carrier in FFT bins.
    float carrier_pos_ = 0;
    bool detected_carrier_ = false;

    // Complex argument at DC frequency
    DeciAngle dc_angle_ = 0;
    DeciAngle prev_dc_angle_;
    float dc_ampl_;

    // The expected frequency offset in bins used as reference for estiamting
    // the clock error.
    float carrier_ref_ = -277800;

    // Estimated clock error.
    float clock_error_;

    // Running average of dc_angle_.
    float avg_dc_angle_ = 0;

    // Running average of dc_ampl_.
    float avg_dc_ampl_ = 0;

    // Number of beacon pulses received.
    // Beacon pulses are used to relate samples of different receivers to each other.
    // Called "pulse" in Python code.
    int32_t beacon_ = -1;

    // Timestamp of last beacon detection
    // struct timeval beacon_timestamp_;

    // Beacon Sample of Arrival
    double soa_ = 0;
    double prev_soa_ = 0;

    // Correlation block within block of data between subsequent beacons.
    // -1: waiting for first pulse
    int32_t cycle_ = -1;

    // Number of samples to skip before and after the SOA of a beacon,
    // i.e. between a beacon pulse and the first correlation block
    int skip_beacon_padding_ = 6000;

    // Synced signal, i.e. signal after carrier recovery.
    FFT synced_fft_calc_;
    complex<float>* synced_signal_;
    complex<float>* synced_fft_;

    // Correlation block size.
    size_t corr_size_;

    // Number of correlatino blocks between beacon pulses.
    int num_cycles_;

    // Correlation block buffers.
    FFT corr_fft_calc_;
    complex<float>* corr_signal_;
    complex<float>* corr_fft_;
    AlignedArray<complex<float>> corrected_corr_fft_;

    // Number of correlation blocks with large phase offsets.
    int num_phase_errors_;

    // Output slice
    int slice_start_;
    int slice_len_;

    // Output stream.
    CorxFileWriter writer_;
};




//// CLI stuff
// TODO: use proper command-line parser (e.g. tclap)

const char *argp_program_version = "array_detector 0.1";
static const char doc[] = "TODO";

#define NUM_EXTRA_OPTIONS 5
static struct argp_option extra_options[] = {
    {"output", 'o', "<FILE>", 0,
        "Output card file\n('-' for stdout)\n[default: no output]", 1},

    // Correlator
    {0, 0, 0, 0, "Correlator settings:", 5},
    {"corr-threshold", 'u', "<constant>c<snr>s", 0,
        "Correlation detection theshold\n[default: 15s]", 5},
    {"template", 'z', "<FILE>", 0,
        "Load template from a .tpl file\n[default: template.tpl]", 5},
    {"rxid", 'r', "<int>", 0,
        "This receiver's unique identifier\n[default: -1]", 5}
};

unique_ptr<fargs_t, decltype(free)*> args = {NULL, free};
std::string output_file;
std::string arg_template_file = "template.tpl";
float arg_corr_thresh_const = 0;
float arg_corr_thresh_snr = 15;
int rxid = -1;

static error_t parse_opt (int key, char *arg, struct argp_state *state) {
    if (key == 'o') {
        output_file = arg;
    } else if (key == 'u') {
        if (!parse_theshold_str(arg,
                                &arg_corr_thresh_const,
                                &arg_corr_thresh_snr)) {
            argp_usage(state);
        }
    } else if (key == 'z') {
        arg_template_file = arg;
    } else if (key == 'r') {
        rxid = atoi(arg);
    } else if (key == ARGP_KEY_ARG) {
        // We don't take any arguments
        argp_usage(state);
    } else {
        int result = fargs_parse_opt(args.get(), key, arg);
        if (result == FARGS_UNKNOWN) {
            return ARGP_ERR_UNKNOWN;
        } else if (result == FARGS_INVALID_VALUE) {
            argp_usage(state);
        }
    }

    return 0;
}

std::unique_ptr<ArrayDetector> detector;

void signal_handler(int signo) {
    (void)signo;  // unused
    if (detector) {
        detector->cancel();
    }
}

int main(int argc, char **argv) {
    // Argument parsing mess
    struct argp_option options[FARGS_NUM_OPTIONS + NUM_EXTRA_OPTIONS];
    memcpy(options,
           extra_options,
           sizeof(struct argp_option)*NUM_EXTRA_OPTIONS);
    memcpy(options + NUM_EXTRA_OPTIONS,
           fargs_options,
           sizeof(struct argp_option)*FARGS_NUM_OPTIONS);
    struct argp argp = {options, parse_opt, NULL,
                        doc, NULL, NULL, NULL};

    args.reset(fargs_new());
    argp_parse(&argp, argc, argv, 0, 0, 0);

    detector.reset(new ArrayDetector(args.get(),
                                     arg_template_file,
                                     arg_corr_thresh_const,
                                     arg_corr_thresh_snr,
                                     1024,
                                     CFile(output_file)));

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);
    signal(SIGPIPE, signal_handler);

    try {
        detector->start();
        while (detector->next()) {
        }
    } catch (FastcardException& e) {
        cerr << e.what() << endl;
        return e.getCode();
    } catch (std::exception& e) {
        cerr << e.what() << endl;
        return -1;
    }

    return 0;
}
