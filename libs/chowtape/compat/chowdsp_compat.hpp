// SPDX-License-Identifier: LGPL-2.1-or-later
// Minimal chowdsp compatibility types for JUCE-free compilation of ChowTape DSP.
// Covers: DelayLine (Lagrange3rd), FIRFilter, IIRFilter, QValCalcs,
//         SVFLowpass, Noise, LevelDetector, Math::sign.
#pragma once

#include "juce_compat.hpp"

namespace chowdsp {

// ---------------------------------------------------------------------------
// Math::sign — scalar sign function, mirrors chowdsp::Math::sign
// ---------------------------------------------------------------------------
namespace Math {
    template <typename T> inline int sign(T x) noexcept {
        return (x > T(0)) - (x < T(0));
    }
} // namespace Math

// ---------------------------------------------------------------------------
// DelayLine — circular buffer with Lagrange 3rd-order interpolation.
// ---------------------------------------------------------------------------
namespace DelayLineInterpolationTypes {
    struct Lagrange3rd {};
    struct None {};
} // namespace DelayLineInterpolationTypes

template <typename T, typename Interpolation = DelayLineInterpolationTypes::Lagrange3rd>
class DelayLine {
  public:
    explicit DelayLine(int maxLengthSamples = 0) {
        if (maxLengthSamples > 0)
            totalSize_ = (size_t)maxLengthSamples + 1;
    }

    void prepare(const dsp::ProcessSpec& spec) {
        numChannels_ = (int)spec.numChannels;
        buffer_.assign(numChannels_, std::vector<T>(totalSize_, T(0)));
        writePos_.assign(numChannels_, 0);
        delay_ = 0.0f;
    }

    void free() {
        buffer_.clear();
        writePos_.clear();
    }
    void reset() {
        for (auto& ch : buffer_)
            std::fill(ch.begin(), ch.end(), T(0));
    }

    void  setDelay(float newDelayInSamples) noexcept { delay_ = newDelayInSamples; }
    float getDelay() const noexcept { return delay_; }

    void pushSample(int ch, T sample) noexcept {
        auto& buf = buffer_[(size_t)ch];
        writePos_[(size_t)ch] =
            (writePos_[(size_t)ch] == 0) ? (int)totalSize_ - 1 : writePos_[(size_t)ch] - 1;
        buf[(size_t)writePos_[(size_t)ch]] = sample;
    }

    T popSample(int ch, float delayInSamples = -1.0f, bool /*updateReadPointer*/ = true) noexcept {
        if (delayInSamples < 0.0f)
            delayInSamples = delay_;
        if constexpr (std::is_same_v<Interpolation, DelayLineInterpolationTypes::Lagrange3rd>)
            return lagrange3(ch, delayInSamples);
        else
            return buffer_[(size_t)ch][readIdx(ch, (int)std::round(delayInSamples))];
    }

    void incrementReadPointer(int /*ch*/) noexcept {}

    template <typename Context> void process(Context&& ctx) noexcept {
        auto& block = ctx.getOutputBlock();
        for (size_t ch = 0; ch < block.getNumChannels(); ++ch) {
            auto* p = block.getChannelPointer(ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i) {
                pushSample((int)ch, p[i]);
                p[i] = popSample((int)ch);
            }
        }
    }

  private:
    size_t totalSize_   = 1 << 21;
    int    numChannels_ = 0;
    float  delay_       = 0.0f;

    std::vector<std::vector<T>> buffer_;
    std::vector<int>            writePos_;

    size_t readIdx(int ch, int delaySamples) const noexcept {
        auto wp = writePos_[(size_t)ch];
        return (size_t)(((size_t)wp + (size_t)delaySamples) % totalSize_);
    }

    T lagrange3(int ch, float d) const noexcept {
        const auto& buf = buffer_[(size_t)ch];
        auto        wp  = writePos_[(size_t)ch];
        auto        n   = (int)totalSize_;

        int   di   = (int)d;
        float frac = d - (float)di;

        auto tap = [&](int offset) -> T {
            return buf[(size_t)(((size_t)wp + (size_t)(di + offset)) % (size_t)n)];
        };
        T y0 = tap(-1), y1 = tap(0), y2 = tap(1), y3 = tap(2);

        float f0 = frac + 1.0f, f1 = frac, f2 = frac - 1.0f, f3 = frac - 2.0f;
        T     c0 = T(-f1 * f2 * f3 / 6.0f);
        T     c1 = T(f0 * f2 * f3 / 2.0f);
        T     c2 = T(-f0 * f1 * f3 / 2.0f);
        T     c3 = T(f0 * f1 * f2 / 6.0f);
        return c0 * y0 + c1 * y1 + c2 * y2 + c3 * y3;
    }
};

// ---------------------------------------------------------------------------
// FIRFilter — direct-form convolution, per-sample or block processing.
// ---------------------------------------------------------------------------
template <typename T, int fixedOrder = -1, size_t maxChannelCount = 2> class FIRFilter {
  public:
    FIRFilter() = default;
    explicit FIRFilter(int order) { setOrder(order); }

    void setOrder(int newOrder) {
        order_ = newOrder;
        coeffs_.assign((size_t)order_, T(0));
    }

    int getOrder() const noexcept { return order_; }

    void prepare(int numChannels) {
        numChannels_ = numChannels;
        state_.assign((size_t)numChannels * (size_t)order_ * 2, T(0));
        zPtr_.assign((size_t)numChannels, 0);
    }

    void reset() noexcept {
        std::fill(state_.begin(), state_.end(), T(0));
        std::fill(zPtr_.begin(), zPtr_.end(), 0);
    }

    void setCoefficients(const T* data) noexcept {
        std::copy(data, data + (size_t)order_, coeffs_.begin());
    }

    T processSample(T x, int ch = 0) noexcept {
        int    halfLen = order_;
        size_t base    = (size_t)ch * (size_t)halfLen * 2;
        T*     z       = state_.data() + base;
        int&   wp      = zPtr_[(size_t)ch];

        z[wp] = z[(size_t)(wp + halfLen)] = x;
        wp                                = (wp == 0) ? halfLen - 1 : wp - 1;

        T        acc = T(0);
        const T* c   = coeffs_.data();
        const T* s   = z + (size_t)wp + 1;
        for (int i = 0; i < halfLen; ++i)
            acc += c[i] * s[i];
        return acc;
    }

    void processBlock(AudioBuffer<T>& buf) {
        for (int ch = 0; ch < buf.getNumChannels(); ++ch) {
            auto* p = buf.getWritePointer(ch);
            for (int i = 0; i < buf.getNumSamples(); ++i)
                p[i] = processSample(p[i], ch);
        }
    }

    void processBlockBypassed(AudioBuffer<T>& buf) {
        for (int ch = 0; ch < buf.getNumChannels(); ++ch) {
            auto* p = buf.getWritePointer(ch);
            for (int i = 0; i < buf.getNumSamples(); ++i) {
                T in = p[i];
                processSample(T(0), ch);
                p[i] = in;
            }
        }
    }

  private:
    int              order_       = 0;
    int              numChannels_ = 0;
    std::vector<T>   coeffs_;
    std::vector<T>   state_;
    std::vector<int> zPtr_;
};

// ---------------------------------------------------------------------------
// IIRFilter<Order> — biquad (chowdsp API, distinct from dsp::IIR::Filter).
// Used by DCBlocker. setCoefs(b[3], a[3]) where a[0]=1 implicitly.
// ---------------------------------------------------------------------------
template <int Order> class IIRFilter {
  public:
    IIRFilter() {
        std::fill(b_, b_ + Order + 1, 0.0f);
        std::fill(a_, a_ + Order + 1, 0.0f);
    }

    void setCoefs(const float* b, const float* a) noexcept {
        for (int i = 0; i <= Order; ++i)
            b_[i] = b[i];
        for (int i = 0; i <= Order; ++i)
            a_[i] = a[i];
        z_[0] = z_[1] = 0.0f;
    }

    void reset() noexcept { z_[0] = z_[1] = 0.0f; }

    float processSample(float x) noexcept {
        float y = b_[0] * x + z_[0];
        z_[0]   = b_[1] * x - a_[1] * y + z_[1];
        z_[1]   = b_[2] * x - a_[2] * y;
        return y;
    }

    void processBlock(float* buf, int numSamples) noexcept {
        for (int i = 0; i < numSamples; ++i)
            buf[i] = processSample(buf[i]);
    }

  private:
    float b_[Order + 1]{};
    float a_[Order + 1]{};
    float z_[2]{};
};

// ---------------------------------------------------------------------------
// QValCalcs — Butterworth pole Q values for a given filter order.
// butterworth_Qs<T, N>() returns std::array<T, N/2> of Q values.
// ---------------------------------------------------------------------------
namespace QValCalcs {
    template <typename T, int N> static std::array<T, N / 2> butterworth_Qs() noexcept {
        std::array<T, N / 2> result;
        for (int k = 0; k < N / 2; ++k) {
            double angle      = MathConstants<double>::pi * (2.0 * k + 1) / (2.0 * N);
            result[(size_t)k] = T(0.5 / std::cos(angle));
        }
        return result;
    }
} // namespace QValCalcs

// ---------------------------------------------------------------------------
// SVFLowpass — Cytomic state-variable filter, Lowpass output.
// ---------------------------------------------------------------------------
template <typename T> class SVFLowpass {
  public:
    void prepare(const dsp::ProcessSpec& spec) {
        fs_ = (T)spec.sampleRate;
        ic1_.assign(spec.numChannels, T(0));
        ic2_.assign(spec.numChannels, T(0));
        updateCoeffs();
    }

    void setCutoffFrequency(T fc) noexcept {
        fc_ = fc;
        updateCoeffs();
    }
    void setQValue(T q) noexcept {
        q_ = q;
        updateCoeffs();
    }
    void reset() noexcept {
        std::fill(ic1_.begin(), ic1_.end(), T(0));
        std::fill(ic2_.begin(), ic2_.end(), T(0));
    }

    T processSample(int ch, T x) noexcept {
        T v1 = (x - ic2_[(size_t)ch]) * g_ * k_ - ic1_[(size_t)ch];
        v1 /= (T(1) + g_ * (g_ + k_));
        T v2 = ic2_[(size_t)ch] + g_ * v1;
        ic1_[(size_t)ch] += T(2) * (g_ + k_) * v1;
        ic2_[(size_t)ch] += T(2) * v2;
        return v2;
    }

  private:
    T              fs_ = T(48000), fc_ = T(10), q_ = T(std::sqrt(0.5));
    T              g_ = T(0), k_ = T(0);
    std::vector<T> ic1_, ic2_;

    void updateCoeffs() noexcept {
        g_ = std::tan(MathConstants<T>::pi * fc_ / fs_);
        k_ = T(1) / q_;
    }
};

// ---------------------------------------------------------------------------
// Noise<T> — Gaussian white noise source.
// ---------------------------------------------------------------------------
template <typename T> class Noise {
  public:
    enum NoiseType { Uniform, Normal, Pink };

    void setNoiseType(NoiseType t) noexcept { type_ = t; }
    void setGainLinear(T g) noexcept { gain_ = g; }
    void prepare(const dsp::ProcessSpec& /*spec*/) noexcept { rng_.seed(std::random_device{}()); }
    void reset() noexcept {}

    template <typename Context> void process(Context&& ctx) noexcept {
        auto& block = ctx.getOutputBlock();
        for (size_t ch = 0; ch < block.getNumChannels(); ++ch) {
            auto* p = block.getChannelPointer(ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i)
                p[i] += nextSample();
        }
    }

    void fillBuffer(T* dst, int numSamples) noexcept {
        for (int i = 0; i < numSamples; ++i)
            dst[i] = nextSample();
    }

  private:
    NoiseType                             type_ = Normal;
    T                                     gain_ = T(1);
    std::mt19937                          rng_;
    std::normal_distribution<float>       normal_{0.0f, 1.0f};
    std::uniform_real_distribution<float> uniform_{-1.0f, 1.0f};

    T nextSample() noexcept {
        float v = (type_ == Normal) ? normal_(rng_) : uniform_(rng_);
        return T(v) * gain_;
    }
};

// ---------------------------------------------------------------------------
// LevelDetector — simple peak follower
// ---------------------------------------------------------------------------
template <typename T> class LevelDetector {
  public:
    void prepare(const dsp::ProcessSpec& spec) { fs_ = (T)spec.sampleRate; }

    void setParameters(T attackMs, T releaseMs) noexcept {
        attackCoeff_  = std::exp(T(-1) / (T(0.001) * attackMs * fs_));
        releaseCoeff_ = std::exp(T(-1) / (T(0.001) * releaseMs * fs_));
    }

    T processSample(T x) noexcept {
        T xAbs = std::abs(x);
        T c    = (xAbs > level_) ? attackCoeff_ : releaseCoeff_;
        level_ = c * level_ + (T(1) - c) * xAbs;
        return level_;
    }

    void reset() noexcept { level_ = T(0); }

  private:
    T fs_           = T(48000);
    T attackCoeff_  = T(0);
    T releaseCoeff_ = T(0);
    T level_        = T(0);
};

// SIMDUtils namespace — stubs for non-SIMD scalar path
namespace SIMDUtils {
    inline float gainToDecibels(float x) noexcept {
        return Decibels::gainToDecibels(x);
    }
    inline float decibelsToGain(float x) noexcept {
        return Decibels::decibelsToGain(x);
    }
} // namespace SIMDUtils

} // namespace chowdsp

// ---------------------------------------------------------------------------
// dsp::DelayLine — alias chowdsp::DelayLine into the dsp:: namespace.
// Used by InputFilters.h and AzimuthProc.h which reference dsp::DelayLine.
// ---------------------------------------------------------------------------
namespace dsp {
template <typename T, typename Interp = chowdsp::DelayLineInterpolationTypes::Lagrange3rd>
using DelayLine = chowdsp::DelayLine<T, Interp>;

namespace DelayLineInterpolationTypes = chowdsp::DelayLineInterpolationTypes;
} // namespace dsp
