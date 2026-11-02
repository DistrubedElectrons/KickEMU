#include "instruments/SignalAnalysis.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace kickemu::signal
{
    double toDb (double v)
    {
        v = std::abs (v);
        return v > 1.0e-10 ? 20.0 * std::log10 (v) : -200.0;
    }

    Measurement measure (const float* x, int n, double sampleRate)
    {
        Measurement m;
        m.samples = n;
        if (n <= 0)
            return m;

        double sum = 0.0, sumSq = 0.0;
        m.min = m.max = x[0];
        for (int i = 0; i < n; ++i)
        {
            const double v = x[i];
            sum += v;
            sumSq += v * v;
            m.min = std::min (m.min, v);
            m.max = std::max (m.max, v);
        }
        m.mean = sum / n;
        m.rms = std::sqrt (sumSq / n);
        m.peak = std::max (std::abs (m.min), std::abs (m.max));
        m.acRms = std::sqrt (std::max (0.0, sumSq / n - m.mean * m.mean));

        // Rising crossings of the mean. The Schmitt band keeps noise from adding
        // crossings; a signal with no AC part has no frequency.
        if (m.acRms < 1.0e-7 * std::max (1.0, m.peak) || sampleRate <= 0.0)
            return m;
        const double hyst = 0.1 * m.acRms;
        bool armed = false;
        int count = 0;
        double first = 0.0, last = 0.0;
        for (int i = 1; i < n; ++i)
        {
            const double a = x[i - 1] - m.mean, b = x[i] - m.mean;
            if (b < -hyst)
                armed = true;
            if (armed && a <= 0.0 && b > 0.0)
            {
                const double t = (i - 1) + (-a) / (b - a);
                if (count == 0)
                    first = t;
                last = t;
                ++count;
                armed = false;
            }
        }
        if (count >= 2 && last > first)
            m.frequency = (count - 1) * sampleRate / (last - first);
        return m;
    }

    void fft (std::vector<std::complex<float>>& a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap (a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double ang = -2.0 * juce::MathConstants<double>::pi / (double) len;
            const std::complex<double> wl (std::cos (ang), std::sin (ang));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<double> w (1.0, 0.0);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = a[i + k];
                    const auto v = a[i + k + len / 2] * std::complex<float> ((float) w.real(), (float) w.imag());
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    std::vector<float> powerSpectrum (const float* x, int n, int fftSize)
    {
        jassert (juce::isPowerOfTwo (fftSize));
        const int bins = fftSize / 2 + 1;
        std::vector<float> power ((size_t) bins, 0.0f);
        if (fftSize < 2)
            return power;

        std::vector<float> window ((size_t) fftSize);
        double wsum = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / fftSize));
            wsum += window[(size_t) i];
        }
        // A sine of amplitude A gives |X| = A * wsum / 2 at its bin: scale to A^2.
        const double scale = 4.0 / (wsum * wsum);

        const int hop = fftSize / 2;
        int blocks = 0;
        std::vector<std::complex<float>> buf ((size_t) fftSize);
        std::vector<double> acc ((size_t) bins, 0.0);
        // Blocks are aligned to the end of the input (the newest samples count).
        int start = n >= fftSize ? (n - fftSize) % hop : 0;
        do
        {
            for (int i = 0; i < fftSize; ++i)
            {
                const int k = start + i;
                buf[(size_t) i] = { k < n ? x[k] * window[(size_t) i] : 0.0f, 0.0f };
            }
            fft (buf);
            for (int b = 0; b < bins; ++b)
                acc[(size_t) b] += std::norm (buf[(size_t) b]);
            ++blocks;
            start += hop;
        } while (start + fftSize <= n);

        for (int b = 0; b < bins; ++b)
            power[(size_t) b] = (float) (acc[(size_t) b] * scale / blocks);
        return power;
    }

    Spectrum spectrum (const float* x, int n, double sampleRate, int fftSize)
    {
        Spectrum s;
        s.sampleRate = sampleRate;
        s.fftSize = fftSize;
        auto p = powerSpectrum (x, n, fftSize);
        s.db.resize (p.size());
        for (size_t i = 0; i < p.size(); ++i)
            s.db[i] = (float) (p[i] > 1.0e-20f ? 10.0 * std::log10 (p[i]) : -200.0);
        return s;
    }

    std::vector<Peak> findPeaks (const Spectrum& s, int maxPeaks, double minDb)
    {
        struct Cand { int bin; Peak p; };
        std::vector<Cand> cands;
        const int bins = (int) s.db.size();
        for (int k = 2; k + 1 < bins; ++k)
        {
            const double a = s.db[(size_t) k - 1], b = s.db[(size_t) k], c = s.db[(size_t) k + 1];
            if (b < minDb || b <= a || b < c)
                continue;
            const double den = a - 2.0 * b + c;
            const double p = den != 0.0 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / den) : 0.0;
            cands.push_back ({ k, { (k + p) * s.binHz(), b - 0.25 * (a - c) * p } });
        }
        std::sort (cands.begin(), cands.end(), [] (const Cand& x, const Cand& y) { return x.p.db > y.p.db; });

        std::vector<Cand> kept;
        for (auto& c : cands)
        {
            if ((int) kept.size() >= maxPeaks)
                break;
            bool near = false;
            for (auto& k : kept)
                near |= std::abs (k.bin - c.bin) <= 3;
            if (! near)
                kept.push_back (c);
        }
        std::vector<Peak> out;
        for (auto& k : kept)
            out.push_back (k.p);
        return out;
    }

    MidiText describeMidi (const juce::uint8* d, size_t n)
    {
        MidiText t;
        if (n == 0)
            return t;

        auto noteName = [] (int note) { return juce::MidiMessage::getMidiNoteName (note, true, true, 4) + " (" + juce::String (note) + ")"; };
        auto hexLen = [] (size_t len) { return juce::String ((int) len) + " bytes"; };
        const int status = d[0];
        const int data1 = n > 1 ? d[1] : 0, data2 = n > 2 ? d[2] : 0;

        if (status < 0xf0)
        {
            t.channel = (status & 0x0f) + 1;
            switch (status & 0xf0)
            {
                case 0x80: t.kind = "note_off"; t.text = "Note Off  " + noteName (data1) + " vel " + juce::String (data2); break;
                case 0x90:
                    if (data2 == 0) { t.kind = "note_off"; t.text = "Note Off  " + noteName (data1) + " (vel 0)"; }
                    else { t.kind = "note_on"; t.text = "Note On   " + noteName (data1) + " vel " + juce::String (data2); }
                    break;
                case 0xa0: t.kind = "poly_at"; t.text = "Poly AT   " + noteName (data1) + " " + juce::String (data2); break;
                case 0xb0:
                {
                    t.kind = "cc";
                    juce::String name;
                    switch (data1)
                    {
                        case 1: name = " mod"; break;
                        case 7: name = " volume"; break;
                        case 10: name = " pan"; break;
                        case 11: name = " expression"; break;
                        case 64: name = " sustain"; break;
                        case 120: name = " all sound off"; break;
                        case 121: name = " reset controllers"; break;
                        case 123: name = " all notes off"; break;
                        default: break;
                    }
                    t.text = "CC " + juce::String (data1) + name + " = " + juce::String (data2);
                    break;
                }
                case 0xc0: t.kind = "pc"; t.text = "Program   " + juce::String (data1); break;
                case 0xd0: t.kind = "chan_at"; t.text = "Chan AT   " + juce::String (data1); break;
                case 0xe0:
                {
                    t.kind = "pitch_bend";
                    const int v = (data1 | (data2 << 7)) - 8192;
                    t.text = "Pitch Bend " + juce::String (v);
                    break;
                }
                default: break;
            }
            return t;
        }

        switch (status)
        {
            case 0xf0:
            {
                t.kind = "sysex";
                t.text = "SysEx " + hexLen (n);
                if (n > 1)
                    t.text << ", id " << juce::String::toHexString (d[1]).paddedLeft ('0', 2)
                           << (d[1] == 0 && n > 3 ? " " + juce::String::toHexString (d[2]).paddedLeft ('0', 2) + " " + juce::String::toHexString (d[3]).paddedLeft ('0', 2) : juce::String());
                if (d[n - 1] != 0xf7)
                    t.text << " (unterminated)";
                break;
            }
            case 0xf1: t.kind = "mtc"; t.text = "MTC quarter frame " + juce::String (data1 >> 4) + ":" + juce::String (data1 & 15); break;
            case 0xf2: t.kind = "song_position"; t.text = "Song Position " + juce::String (data1 | (data2 << 7)); break;
            case 0xf3: t.kind = "song_select"; t.text = "Song Select " + juce::String (data1); break;
            case 0xf6: t.kind = "tune_request"; t.text = "Tune Request"; break;
            case 0xf8: t.kind = "clock"; t.text = "Clock"; t.isRealtime = true; break;
            case 0xfa: t.kind = "start"; t.text = "Start"; t.isRealtime = true; break;
            case 0xfb: t.kind = "continue"; t.text = "Continue"; t.isRealtime = true; break;
            case 0xfc: t.kind = "stop"; t.text = "Stop"; t.isRealtime = true; break;
            case 0xfe: t.kind = "active_sensing"; t.text = "Active Sensing"; t.isRealtime = true; break;
            case 0xff: t.kind = "reset"; t.text = "System Reset"; t.isRealtime = true; break;
            default: t.kind = "unknown"; t.text = "Undefined " + juce::String::toHexString (status); break;
        }
        return t;
    }
}
