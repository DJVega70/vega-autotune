#pragma once
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <deque>
#include <complex>

// =====================================================================
//  Vega Productions Autotune — pitch detection (YIN) + PSOLA shifter
// =====================================================================
namespace vat
{
/** YIN fundamental estimator; the difference function is computed with FFT
    cross‑correlation, so cost is O(N log N) per analysis instead of O(N^2). */
class Yin
{
public:
    void prepare (double sampleRate, float minHz, float maxHz)
    {
        fs = sampleRate;
        maxLag = (int) std::ceil (fs / minHz) + 2;
        minLag = std::max (2, (int) std::floor (fs / maxHz));
        win = maxLag;                                   // integration window = longest period
        order = 1; while ((1 << order) < 3 * win) ++order;
        N = 1 << order;
        fft = std::make_unique<juce::dsp::FFT> (order);
        a.assign ((size_t) N, {}); b.assign ((size_t) N, {}); fa.assign ((size_t) N, {}); fb.assign ((size_t) N, {});
        d.assign ((size_t) maxLag + 2, 0.0f); prefix.assign ((size_t) 2 * win + 2, 0.0);
    }
    int samplesNeeded() const { return 2 * win; }

    /** x: the most recent 2*win samples (oldest first). Returns period in samples (0 = unvoiced). */
    float detect (const float* x, float& confidence)
    {
        const int L = 2 * win;
        prefix[0] = 0; for (int i = 0; i < L; ++i) prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) x[i] * x[i];
        const double e0 = prefix[(size_t) win];
        if (e0 / win < 1.0e-7) { confidence = 0; return 0.0f; }     // silence gate (~ -70 dBFS)

        for (int i = 0; i < N; ++i) { a[(size_t) i] = { i < win ? x[i] : 0.0f, 0.0f }; b[(size_t) i] = { i < L ? x[i] : 0.0f, 0.0f }; }
        fft->perform (a.data(), fa.data(), false);
        fft->perform (b.data(), fb.data(), false);
        for (int i = 0; i < N; ++i) fa[(size_t) i] = std::conj (fa[(size_t) i]) * fb[(size_t) i];
        fft->perform (fa.data(), a.data(), true);          // a[tau] = sum_j x[j] x[j+tau]  (scaled by N in JUCE inverse? normalised below)
        const float invN = 1.0f / (float) N;
        // JUCE's inverse FFT is not normalised for complex perform(): detect scaling from tau = 0
        const float r0 = a[0].real();
        const float scale = std::abs (r0) > 1.0e-20f ? (float) e0 / r0 : invN;

        d[0] = 1.0f;
        double running = 0.0;
        for (int tau = 1; tau <= maxLag && tau < win; ++tau)
        {
            const double et = prefix[(size_t) (tau + win)] - prefix[(size_t) tau];
            const double r = a[(size_t) tau].real() * scale;
            const double diff = std::max (0.0, e0 + et - 2.0 * r);
            running += diff;
            d[(size_t) tau] = running > 0 ? (float) (diff * tau / running) : 1.0f;
        }
        const int top = std::min (maxLag, win - 1);
        int best = -1;
        for (int tau = minLag; tau < top; ++tau)
            if (d[(size_t) tau] < threshold)
            {
                while (tau + 1 < top && d[(size_t) tau + 1] < d[(size_t) tau]) ++tau;
                best = tau; break;
            }
        if (best < 0)
        {   // fall back to the global minimum if it is reasonably periodic
            float mn = 1.0f; for (int tau = minLag; tau < top; ++tau) if (d[(size_t) tau] < mn) { mn = d[(size_t) tau]; best = tau; }
            if (mn > 0.35f) { confidence = 0; return 0.0f; }
        }
        // parabolic interpolation
        float p = (float) best;
        if (best > 1 && best < top - 1)
        {
            const float y0 = d[(size_t) best - 1], y1 = d[(size_t) best], y2 = d[(size_t) best + 1];
            const float den = y0 - 2 * y1 + y2;
            if (std::abs (den) > 1.0e-9f) p += 0.5f * (y0 - y2) / den;
        }
        confidence = juce::jlimit (0.0f, 1.0f, 1.0f - d[(size_t) best]);
        return p;
    }
    float threshold = 0.15f;
private:
    double fs = 48000; int minLag = 40, maxLag = 800, win = 800, order = 11, N = 2048;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<std::complex<float>> a, b, fa, fb;
    std::vector<float> d; std::vector<double> prefix;
};

/** Real‑time TD‑PSOLA pitch shifter with formant shift (grain resampling).
    The voice's spectral envelope (formants) is preserved by design, so shifting
    pitch doesn't produce the "chipmunk" effect; the formant control moves it on purpose. */
class Psola
{
public:
    static constexpr int kRing = 1 << 16;

    /** minFormant bounds how far down the formant may go; lower values need more look‑ahead. */
    void prepare (int maxPeriodSamples, int hopSamples, int numCh, float minFormant)
    {
        maxT = maxPeriodSamples; channels = std::min (2, numCh); fMin = minFormant;
        // Output lags input by D: a grain centred on a mark needs one period of input after it,
        // and is laid down Lg/2 = T/f before it. So D >= T * (1 + 1/fMin), plus a hop of slack.
        latency = (int) std::ceil (maxT * (1.0 + 1.0 / fMin)) + hopSamples + 4;
        for (int c = 0; c < 2; ++c) { in[c].assign (kRing, 0.0f); out[c].assign (kRing, 0.0f); }
        lp.assign (kRing, 0.0f);
        reset();
    }
    void reset()
    {
        for (int c = 0; c < 2; ++c) { std::fill (in[c].begin(), in[c].end(), 0.0f); std::fill (out[c].begin(), out[c].end(), 0.0f); }
        std::fill (lp.begin(), lp.end(), 0.0f);
        n = 0; nextAm = 0; nextSm = 0; marks.clear(); lpState = 0;
        period = 0; ratio = 1; formant = 1;
    }
    int getLatency() const { return latency; }

    /** period: detected period in samples (0 = unvoiced); ratio: output/input frequency; formant: spectral envelope scale */
    void setTarget (float periodSamples, float pitchRatio, float formantFactor)
    {
        period = periodSamples; ratio = juce::jlimit (0.25f, 4.0f, pitchRatio); formant = juce::jlimit (fMin, 1.6f, formantFactor);
    }

    /** push one input frame, returns one output frame (delayed by getLatency()). */
    inline void process (const float* inFrame, float* outFrame)
    {
        const int w = (int) (n & (kRing - 1));
        float mono = 0;
        for (int c = 0; c < channels; ++c) { in[c][(size_t) w] = inFrame[c]; mono += inFrame[c]; }
        lpState += 0.12f * (mono - lpState);      // light low‑pass for mark placement
        lp[(size_t) w] = lpState;
        ++n;

        // ---- analysis marks (need one period of input after each mark)
        const bool voiced = period > 1.0f;
        const double T = voiced ? (double) period : (double) unvoicedT;
        while (nextAm + T + T * 0.3 < (double) n)
        {
            double pos = nextAm;
            if (voiced)
            {   // snap mark to the local peak so grains stay phase‑coherent
                const int r = (int) (T * 0.25);
                const juce::int64 centre = (juce::int64) std::llround (pos);
                juce::int64 bestIdx = centre; float bestV = -1.0f;
                for (int k = -r; k <= r; ++k)
                {
                    const juce::int64 idx = centre + k;
                    if (idx < 0 || idx >= n) continue;
                    const float v = std::abs (lp[(size_t) (idx & (kRing - 1))]);
                    if (v > bestV) { bestV = v; bestIdx = idx; }
                }
                pos = (double) bestIdx;
                if (! marks.empty() && pos <= marks.back().pos + T * 0.5) pos = marks.back().pos + T;   // keep spacing sane
            }
            marks.push_back ({ pos, (float) T, voiced });
            nextAm = pos + T;
            while (marks.size() > 64) marks.pop_front();
        }

        // ---- synthesis: lay down grains just before the output reaches them
        const juce::int64 o = n - latency;
        while (! marks.empty())
        {
            const Mark* m = nearest (nextSm);
            if (! m) break;
            const float Ta = m->T;
            const float f = formant;
            const int Lg = std::max (4, (int) std::lround (2.0f * Ta / f));
            if (nextSm - Lg * 0.5 > (double) o + 1.0) break;          // not needed yet
            if (m->pos + Ta >= (double) n) break;                     // source not complete yet (shouldn't happen)
            const float Ts = m->voiced ? Ta / ratio : Ta;
            const float scale = juce::jlimit (0.0f, 4.0f, 2.0f * Ts / (float) Lg);
            const double start = nextSm - Lg * 0.5;
            const double srcStart = m->pos - Ta;
            for (int k = 0; k < Lg; ++k)
            {
                const double dst = start + k;
                if (dst < (double) o) continue;                        // too late for this sample (rare)
                const float win = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (k + 0.5f) / Lg);
                const double sp = srcStart + k * f;
                const juce::int64 s0 = (juce::int64) std::floor (sp); const float fr = (float) (sp - (double) s0);
                const juce::int64 di = (juce::int64) std::floor (dst);
                for (int c = 0; c < channels; ++c)
                {
                    const float a = in[c][(size_t) (s0 & (kRing - 1))], b = in[c][(size_t) ((s0 + 1) & (kRing - 1))];
                    out[c][(size_t) (di & (kRing - 1))] += (a + (b - a) * fr) * win * scale;
                }
            }
            nextSm += Ts;
        }

        // ---- read output
        if (o >= 0)
        {
            const size_t ri = (size_t) (o & (kRing - 1));
            for (int c = 0; c < channels; ++c) { outFrame[c] = out[c][ri]; out[c][ri] = 0.0f; }
        }
        else for (int c = 0; c < channels; ++c) outFrame[c] = 0.0f;
        if (nextSm < (double) o - maxT) nextSm = (double) o;   // recover after silence / big jumps
    }

private:
    struct Mark { double pos; float T; bool voiced; };
    const Mark* nearest (double t) const
    {
        const Mark* best = nullptr; double bd = 1e18;
        for (auto& m : marks) { const double dd = std::abs (m.pos - t); if (dd < bd) { bd = dd; best = &m; } }
        return best;
    }
    std::vector<float> in[2], out[2], lp;
    std::deque<Mark> marks;
    juce::int64 n = 0; double nextAm = 0, nextSm = 0;
    int maxT = 800, latency = 2000, channels = 2, unvoicedT = 256;
    float period = 0, ratio = 1, formant = 1, lpState = 0, fMin = 0.7f;
};
} // namespace vat
