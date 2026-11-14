#include "analysis.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace {

const double kTwoPi = 6.283185307179586;

// Analysis runs on a mono signal decimated to roughly 22 kHz.
const int kFftSize = 1024;
const int kFftHalf = kFftSize / 2;
const int kFftBits = 10;

// The log-compressed flux peaks slightly before the window centre reaches the
// attack. Measured against the demo track's known grid (see --selftest).
const double kOnsetBias = 0.008;

struct Rng {
    uint32_t s;
    float next() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) * (1.0f / 16777216.0f); }
    int below(int n) { return (int)(next() * (float)n) % n; }
};

struct Candidate {
    int   frame;
    float strength;   // onset strength relative to its surroundings
    float centroid;   // spectral centroid of the energy that appeared, in FFT bins
};

float Percentile(std::vector<float> v, double p)
{
    if (v.empty()) return 0.0f;
    size_t k = (size_t)(p * (double)(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// Picks the strongest onsets that respect a minimum gap, then spreads them over four lanes.
void BuildChart(const std::vector<Candidate>& cands, double frameSec, double timeOffset,
                double duration, int diff, Chart& out)
{
    static const double kGap[DIFF_COUNT]         = { 0.42, 0.24, 0.13 };   // min seconds between notes
    static const double kDensity[DIFF_COUNT]     = { 1.2, 2.2, 3.6 };      // target notes per second
    static const float  kMinStrength[DIFF_COUNT] = { 1.5f, 1.25f, 1.1f };
    static const double kChordShare[DIFF_COUNT]  = { 0.0, 0.07, 0.14 };
    static const double kJackGap[DIFF_COUNT]     = { 0.5, 0.26, 0.17 };    // same lane twice faster than this is avoided
    static const int    kMaxStreak[DIFF_COUNT]   = { 3, 2, 2 };

    out.time.clear();
    out.lane.clear();
    if (cands.empty()) return;

    std::vector<int> order(cands.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return cands[a].strength > cands[b].strength; });

    const int gapFrames = std::max(1, (int)std::lround(kGap[diff] / frameSec));
    const int lastFrame = cands.back().frame;
    std::vector<uint8_t> taken((size_t)lastFrame + 1, 0);
    const size_t target = (size_t)(duration * kDensity[diff]);

    std::vector<int> picked;
    for (int idx : order) {
        if (picked.size() >= target) break;
        const Candidate& c = cands[idx];
        if (c.strength < kMinStrength[diff]) break;
        int lo = std::max(0, c.frame - gapFrames + 1);
        int hi = std::min(lastFrame, c.frame + gapFrames - 1);
        bool free = true;
        for (int f = lo; f <= hi; f++) if (taken[f]) { free = false; break; }
        if (!free) continue;
        taken[c.frame] = 1;
        picked.push_back(idx);
    }
    if (picked.empty()) return;
    std::sort(picked.begin(), picked.end());   // candidates are in time order, so index order is time order

    // Lane follows pitch: quartiles of the spectral centroid, so similar sounds land together.
    std::vector<float> cents, strengths;
    cents.reserve(picked.size());
    strengths.reserve(picked.size());
    for (int idx : picked) { cents.push_back(cands[idx].centroid); strengths.push_back(cands[idx].strength); }
    std::vector<float> sorted = cents;
    std::sort(sorted.begin(), sorted.end());
    const float q1 = sorted[sorted.size() / 4];
    const float q2 = sorted[sorted.size() / 2];
    const float q3 = sorted[sorted.size() * 3 / 4];
    const float chordThreshold =
        kChordShare[diff] > 0.0 ? Percentile(strengths, 1.0 - kChordShare[diff]) : 1e30f;

    // The pitch-to-lane mapping is reshuffled every phrase so patterns keep moving.
    static const uint8_t kPerms[6][4] = {
        { 0, 1, 2, 3 }, { 1, 0, 3, 2 }, { 3, 2, 1, 0 }, { 2, 3, 0, 1 }, { 0, 2, 1, 3 }, { 1, 3, 0, 2 },
    };
    Rng rng{ 0x9E3779B9u ^ (uint32_t)picked.size() ^ ((uint32_t)diff << 20) };
    int perm = 0, prevLane = -1, streak = 0;
    double prevTime = -10.0;

    for (size_t k = 0; k < picked.size(); k++) {
        const Candidate& c = cands[picked[k]];
        const double t = std::max(0.0, c.frame * frameSec + timeOffset);
        if (k > 0 && k % 16 == 0) perm = (perm + 1 + rng.below(5)) % 6;

        int band = (c.centroid > q1) + (c.centroid > q2) + (c.centroid > q3);
        int lane = kPerms[perm][band];
        if (lane == prevLane) {
            if (t - prevTime < kJackGap[diff] || streak >= kMaxStreak[diff]) {
                int dir = rng.next() < 0.5f ? -1 : 1;
                int moved = lane + dir;
                if (moved < 0 || moved > 3) moved = lane - dir;
                lane = moved;
                streak = 1;
            } else {
                streak++;
            }
        } else {
            streak = 1;
        }

        out.time.push_back(t);
        out.lane.push_back((uint8_t)lane);

        // Accents with room around them become two-note chords.
        if (c.strength >= chordThreshold) {
            double nextTime = k + 1 < picked.size() ? cands[picked[k + 1]].frame * frameSec + timeOffset : 1e9;
            if (t - prevTime >= 0.22 && nextTime - t >= 0.22) {
                out.time.push_back(t);
                out.lane.push_back((uint8_t)((lane + 2) & 3));
            }
        }
        prevLane = lane;
        prevTime = t;
    }
}

} // namespace

bool AnalyzeSong(const int16_t* pcm, size_t frames, int channels, int sampleRate, SongAnalysis& out)
{
    if (!pcm || channels < 1 || sampleRate < 8000) return false;

    // Mix down to mono and decimate by an integer factor (box filter is enough here).
    const int dec = std::max(1, (int)std::lround(sampleRate / 22050.0));
    const size_t n = frames / (size_t)dec;
    if (n < (size_t)kFftSize * 8) return false;

    // Only one analysis window of mono audio exists at a time; the song itself is read in place.
    const float scale = 1.0f / (32768.0f * (float)dec * (float)channels);
    const int span = dec * channels;
    auto mono = [&](size_t i) {
        const int16_t* p = pcm + i * (size_t)span;
        int sum = 0;
        for (int j = 0; j < span; j++) sum += p[j];
        return (float)sum * scale;
    };
    std::vector<float> frame(kFftSize);

    const double fs = (double)sampleRate / dec;
    const int hop = std::max(64, (int)std::lround(fs * 0.008));
    const size_t nf = (n - kFftSize) / (size_t)hop + 1;
    const double frameSec = hop / fs;

    // FFT tables.
    std::vector<float> window(kFftSize), cosT(kFftHalf), sinT(kFftHalf);
    std::vector<uint16_t> rev(kFftSize);
    for (int i = 0; i < kFftSize; i++) {
        window[i] = (float)(0.5 - 0.5 * std::cos(kTwoPi * i / (kFftSize - 1)));
        int r = 0;
        for (int b = 0; b < kFftBits; b++) if (i & (1 << b)) r |= 1 << (kFftBits - 1 - b);
        rev[i] = (uint16_t)r;
    }
    for (int i = 0; i < kFftHalf; i++) {
        cosT[i] = (float)std::cos(kTwoPi * i / kFftSize);
        sinT[i] = (float)-std::sin(kTwoPi * i / kFftSize);
    }

    // Three bands: bass (kick), mids (snare, vocals, melody), highs (hats, cymbals).
    const double binHz = fs / kFftSize;
    const int edges[4] = {
        1,
        std::max(2, (int)std::lround(180.0 / binHz)),
        (int)std::lround(2500.0 / binHz),
        std::min(kFftHalf - 1, (int)std::lround(10000.0 / binHz)),
    };

    std::vector<float> flux[3], energy[3], centroid(nf);
    for (int b = 0; b < 3; b++) { flux[b].resize(nf); energy[b].resize(nf); }

    std::vector<float> re(kFftSize), im(kFftSize), prev(kFftHalf, 0.0f);
    for (size_t f = 0; f < nf; f++) {
        // Slide the window along: keep the overlap, mix down only the new samples.
        const size_t start = f * (size_t)hop;
        const int fresh = f == 0 || hop >= kFftSize ? kFftSize : hop;
        if (fresh < kFftSize) memmove(frame.data(), frame.data() + fresh, (size_t)(kFftSize - fresh) * sizeof(float));
        for (int i = kFftSize - fresh; i < kFftSize; i++) frame[i] = mono(start + (size_t)i);
        for (int i = 0; i < kFftSize; i++) { re[rev[i]] = frame[i] * window[i]; }
        std::fill(im.begin(), im.end(), 0.0f);

        for (int size = 2; size <= kFftSize; size <<= 1) {
            const int h = size >> 1, step = kFftSize / size;
            for (int s = 0; s < kFftSize; s += size) {
                for (int j = 0, k = 0; j < h; j++, k += step) {
                    const int a = s + j, b = a + h;
                    const float tr = re[b] * cosT[k] - im[b] * sinT[k];
                    const float ti = re[b] * sinT[k] + im[b] * cosT[k];
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                }
            }
        }

        // Spectral flux: how much log-magnitude appeared since the previous frame.
        float centSum = 0.0f, centTotal = 0.0f;
        for (int b = 0; b < 3; b++) {
            float fl = 0.0f, en = 0.0f;
            for (int k = edges[b]; k < edges[b + 1]; k++) {
                const float m = std::log1p(std::sqrt(re[k] * re[k] + im[k] * im[k]));
                const float d = m - prev[k];
                prev[k] = m;
                en += m;
                if (d > 0.0f) { fl += d; centSum += d * (float)k; centTotal += d; }
            }
            flux[b][f] = fl;
            energy[b][f] = en;
        }
        centroid[f] = centTotal > 0.0f ? centSum / centTotal : 0.0f;
    }

    // Onset candidates: local maxima of each band's flux that stand out from the local average.
    static const float kBandWeight[3] = { 1.25f, 1.0f, 0.85f };
    const int halfWin = std::max(4, (int)std::lround(0.5 / frameSec));
    std::vector<Candidate> raw;
    for (int b = 0; b < 3; b++) {
        const std::vector<float>& F = flux[b];
        std::vector<double> prefix(nf + 1, 0.0);
        for (size_t i = 0; i < nf; i++) prefix[i + 1] = prefix[i] + F[i];
        const double globalMean = prefix[nf] / (double)nf;
        if (globalMean <= 1e-9) continue;
        for (size_t i = 2; i + 2 < nf; i++) {
            const float v = F[i];
            if (v <= F[i - 1] || v < F[i + 1] || v <= F[i - 2] || v < F[i + 2]) continue;
            const size_t lo = i > (size_t)halfWin ? i - halfWin : 0;
            const size_t hi = std::min(nf, i + (size_t)halfWin + 1);
            const double localMean = (prefix[hi] - prefix[lo]) / (double)(hi - lo);
            const float s = (float)(v / (localMean + 0.5 * globalMean)) * kBandWeight[b];
            if (s > 1.0f) raw.push_back({ (int)i, s, centroid[i] });
        }
    }
    std::sort(raw.begin(), raw.end(), [](const Candidate& a, const Candidate& b) { return a.frame < b.frame; });

    // Onsets that several bands report within 45 ms are one event.
    const int mergeFrames = std::max(1, (int)std::lround(0.045 / frameSec));
    std::vector<Candidate> cands;
    for (size_t i = 0; i < raw.size();) {
        Candidate best = raw[i];
        float extra = 0.0f;
        size_t j = i + 1;
        for (; j < raw.size() && raw[j].frame - raw[i].frame <= mergeFrames; j++) {
            if (raw[j].strength > best.strength) { extra += best.strength; best = raw[j]; }
            else extra += raw[j].strength;
        }
        best.strength += 0.35f * extra;
        cands.push_back(best);
        i = j;
    }

    out.duration  = (double)frames / sampleRate;
    out.hop       = frameSec;
    out.envOffset = (kFftSize / 2) / fs;
    const double noteOffset = out.envOffset + kOnsetBias;
    for (int d = 0; d < DIFF_COUNT; d++)
        BuildChart(cands, frameSec, noteOffset, out.duration, d, out.charts[d]);

    // Envelopes for the visuals, normalized so typical loud passages reach about 1.
    std::vector<float>* dst[3] = { &out.low, &out.mid, &out.high };
    for (int b = 0; b < 3; b++) {
        const float ref = std::max(1e-6f, Percentile(energy[b], 0.95));
        dst[b]->resize(nf);
        for (size_t i = 0; i < nf; i++) (*dst[b])[i] = std::min(1.3f, energy[b][i] / ref);
    }
    {
        const float ref = std::max(1e-6f, Percentile(flux[0], 0.99));
        out.pulse.resize(nf);
        for (size_t i = 0; i < nf; i++) out.pulse[i] = std::min(1.0f, flux[0][i] / ref);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Demo track
// ---------------------------------------------------------------------------

namespace {
const int    kDemoRate = 44100;
const double kDemoBpm  = 124.0;
}

double DemoSixteenthSeconds() { return 60.0 / kDemoBpm / 4.0; }

void SynthDemoTrack(std::vector<int16_t>& stereo, int& sampleRate)
{
    const int sr = kDemoRate;
    const double beat = 60.0 / kDemoBpm, six = beat / 4.0;
    const int bars = 24;
    const size_t len = (size_t)((bars * 4 * beat + 2.5) * sr);
    std::vector<float> out(len, 0.0f);
    Rng rng{ 987654321u };

    auto kick = [&](double t, float g) {
        size_t s = (size_t)(t * sr), n = (size_t)(0.32 * sr);
        double ph = 0.0;
        for (size_t i = 0; i < n && s + i < len; i++) {
            double tt = (double)i / sr;
            ph += kTwoPi * (48.0 + 140.0 * std::exp(-tt * 30.0)) / sr;
            out[s + i] += (float)(std::sin(ph) * std::exp(-tt * 8.0) * g * std::min(1.0, i / 40.0));
        }
    };
    // Noise burst through a one-pole high-pass; `tone` adds a pitched body (snare).
    auto noise = [&](double t, float g, double decay, float hp, double tone) {
        size_t s = (size_t)(t * sr), n = (size_t)(std::min(0.4, 6.0 / decay) * sr);
        float lp = 0.0f;
        for (size_t i = 0; i < n && s + i < len; i++) {
            double tt = (double)i / sr;
            float w = rng.next() * 2.0f - 1.0f;
            lp += (w - lp) * hp;
            double v = (w - lp);
            if (tone > 0.0) v += 0.7 * std::sin(kTwoPi * tone * tt) * std::exp(-tt * 14.0);
            out[s + i] += (float)(v * std::exp(-tt * decay) * g);
        }
    };
    // kind 0: bass, 1: plucked arpeggio, 2: slow pad
    auto tone = [&](double t, double dur, double freq, float g, int kind) {
        size_t s = (size_t)(t * sr), n = (size_t)((dur + 0.2) * sr);
        const double w = kTwoPi * freq / sr;
        for (size_t i = 0; i < n && s + i < len; i++) {
            double tt = (double)i / sr, ph = w * (double)i, v, env;
            double release = tt < dur ? 1.0 : std::exp(-(tt - dur) * 25.0);
            if (kind == 0) {
                v = std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.3 * std::sin(3 * ph) + 0.15 * std::sin(4 * ph);
                env = std::min(1.0, tt * 300.0) * std::exp(-tt * 3.0) * release;
            } else if (kind == 1) {
                v = std::sin(ph + 1.8 * std::exp(-tt * 9.0) * std::sin(2 * ph));
                env = std::min(1.0, tt * 400.0) * std::exp(-tt * 7.0) * release;
            } else {
                v = std::sin(ph) + std::sin(ph * 1.006) + 0.6 * std::sin(ph * 0.5);
                env = std::min(1.0, tt * 1.2) * release;
            }
            out[s + i] += (float)(v * env * g);
        }
    };

    static const double kRoots[4]  = { 110.0, 87.31, 130.81, 98.0 };          // Am, F, C, G
    static const double kThirds[4] = { 1.1892, 1.2599, 1.2599, 1.2599 };
    static const int    kArp[8]    = { 0, 2, 1, 2, 3, 2, 1, 2 };

    for (int bar = 0; bar < bars; bar++) {
        // 0 intro, 1 build, 2 drop, 3 break, 4 second drop
        const int sec = bar < 4 ? 0 : bar < 8 ? 1 : bar < 16 ? 2 : bar < 20 ? 3 : 4;
        const bool drop = sec == 2 || sec == 4;
        const int ci = bar & 3;
        const double root = kRoots[ci];
        const double chord[4] = { 1.0, kThirds[ci], 1.4983, 2.0 };
        const double tb = bar * 4 * beat;

        for (int s = 0; s < 16; s++) {
            const double t = tb + s * six;
            if ((sec == 1 || drop) && s % 4 == 0) kick(t, 0.9f);
            if (drop && (s == 4 || s == 12)) noise(t, 0.34f, 20.0, 0.18f, 190.0);
            if (sec == 0 && s % 4 == 2) noise(t, 0.10f, 70.0, 0.6f, 0.0);
            if (sec == 1 && s % 2 == 0) noise(t, s % 4 == 2 ? 0.14f : 0.07f, 70.0, 0.6f, 0.0);
            if (drop) noise(t, s % 4 == 2 ? 0.16f : 0.06f, s % 4 == 2 ? 40.0 : 90.0, 0.6f, 0.0);
            if ((sec == 1 || drop) && (s == 2 || s == 3 || s == 6 || s == 10 || s == 11 || s == 14))
                tone(t, six * 0.8, root * 0.5, 0.26f, 0);
            const bool arpOn = drop ? true : sec == 3 ? s % 4 == 0 : s % 2 == 0;
            if (arpOn) tone(t, six * 0.9, root * 4.0 * chord[kArp[s % 8]], drop ? 0.13f : 0.16f, 1);
        }
        if (sec == 0 || sec == 3) {
            tone(tb, 4 * beat, root * 2.0, 0.05f, 2);
            tone(tb, 4 * beat, root * 2.0 * chord[1], 0.04f, 2);
            tone(tb, 4 * beat, root * 2.0 * chord[2], 0.04f, 2);
        }
        if (bar == 7 || bar == 19) {   // snare roll into each drop
            for (int s = 8; s < 16; s++)
                noise(tb + s * six, 0.10f + 0.03f * (s - 8), 28.0, 0.2f, 190.0);
        }
    }

    sampleRate = sr;
    stereo.resize(len * 2);
    for (size_t i = 0; i < len; i++) {
        const int16_t v = (int16_t)(std::tanh(out[i] * 1.1f) * 0.9f * 32767.0f);
        stereo[2 * i] = v;
        stereo[2 * i + 1] = v;
    }
}
