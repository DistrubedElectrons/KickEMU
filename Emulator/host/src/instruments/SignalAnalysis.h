#pragma once

#include <juce_core/juce_core.h>
#include <complex>
#include <vector>

namespace kickemu::signal
{
    /** Time-domain readings of a block of samples (the multimeter). */
    struct Measurement
    {
        int samples = 0;
        double mean = 0.0;       // DC
        double rms = 0.0;        // total RMS, DC included
        double acRms = 0.0;      // RMS with the mean removed
        double peak = 0.0;       // largest |x|
        double min = 0.0, max = 0.0;
        double frequency = 0.0;  // Hz from rising crossings of the mean, 0 = none found
    };

    /** Frequency: rising crossings of the mean with a Schmitt trigger at +-10 % of the AC RMS,
        crossing instants interpolated between samples; needs at least two crossings. */
    Measurement measure (const float* x, int n, double sampleRate);

    /** 20 log10 (|v|), floored at -200 dB. */
    double toDb (double v);

    /** Power spectrum in dBFS: a full-scale sine reads 0 dB at its bin (Hann window,
        coherent gain corrected). Welch average of 50 %-overlapped blocks of fftSize
        over the whole input; shorter input is zero padded. */
    struct Spectrum
    {
        double sampleRate = 0.0;
        int fftSize = 0;
        std::vector<float> db;   // fftSize / 2 + 1 bins
        double binHz() const { return fftSize > 0 ? sampleRate / fftSize : 0.0; }
    };
    Spectrum spectrum (const float* x, int n, double sampleRate, int fftSize);

    /** Linear power (|X|^2 normalised as above) per bin, for averaging in the UI. */
    std::vector<float> powerSpectrum (const float* x, int n, int fftSize);

    struct Peak
    {
        double frequency = 0.0;
        double db = 0.0;
    };
    /** Strongest local maxima above minDb, frequency and level refined by parabolic
        interpolation; peaks within 3 bins of a stronger one are dropped. DC is skipped. */
    std::vector<Peak> findPeaks (const Spectrum& s, int maxPeaks, double minDb = -140.0);

    /** In-place radix-2 FFT (size must be a power of two). */
    void fft (std::vector<std::complex<float>>& data);

    /** Decoded MIDI message, for the monitor. */
    struct MidiText
    {
        juce::String kind;   // note_on, note_off, cc, pc, pitch_bend, poly_at, chan_at, sysex, clock, ...
        int channel = 0;     // 1..16, 0 for system messages
        juce::String text;   // "Note On  C4 (60) vel 100"
        bool isRealtime = false;
    };
    MidiText describeMidi (const juce::uint8* data, size_t size);
}
