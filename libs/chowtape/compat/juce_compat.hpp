// SPDX-License-Identifier: LGPL-2.1-or-later
// Minimal JUCE compatibility shim for JUCE-free compilation of ChowTape DSP.
// Covers only the surface actually used by the vendored processors.
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <vector>

// ---------------------------------------------------------------------------
// Basic JUCE typedefs
// ---------------------------------------------------------------------------
using uint32 = uint32_t;
using int64  = int64_t;

// ---------------------------------------------------------------------------
// Warning suppression macros — no-op outside JUCE
// ---------------------------------------------------------------------------
#define JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE(...)
#define JUCE_END_IGNORE_WARNINGS_GCC_LIKE(...)
#define JUCE_BEGIN_IGNORE_WARNINGS_MSVC(...)
#define JUCE_END_IGNORE_WARNINGS_MSVC(...)

// ---------------------------------------------------------------------------
// Non-copyable macro
// ---------------------------------------------------------------------------
#define JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClassName)                                    \
    ClassName(const ClassName&)            = delete;                                               \
    ClassName& operator=(const ClassName&) = delete;

// ---------------------------------------------------------------------------
// Assertion + range helpers
// ---------------------------------------------------------------------------
#define jassert(x) assert(x)

template <typename T> inline bool isPositiveAndBelow(T val, T upper) noexcept {
    return val >= T(0) && val < upper;
}

// ---------------------------------------------------------------------------
// Math helpers
// ---------------------------------------------------------------------------
template <typename T> struct MathConstants {
    static constexpr T pi    = static_cast<T>(3.14159265358979323846);
    static constexpr T twoPi = static_cast<T>(6.28318530717958647692);
};

template <typename T> inline T jlimit(T lo, T hi, T val) {
    return std::clamp(val, lo, hi);
}

template <typename T> inline T jmax(T a, T b) {
    return std::max(a, b);
}

template <typename T> inline T jmin(T a, T b) {
    return std::min(a, b);
}

// ---------------------------------------------------------------------------
// Decibels
// ---------------------------------------------------------------------------
struct Decibels {
    static float gainToDecibels(float gain) noexcept {
        return gain > 0.0f ? 20.0f * std::log10(gain) : -100.0f;
    }
    static float decibelsToGain(float dB) noexcept { return std::pow(10.0f, dB / 20.0f); }
};

// ---------------------------------------------------------------------------
// AudioBuffer<T>
// ---------------------------------------------------------------------------
template <typename T> class AudioBuffer {
  public:
    AudioBuffer() = default;

    AudioBuffer(int numChannels, int numSamples) { setSize(numChannels, numSamples); }

    void setSize(int numChannels, int numSamples, bool keepExistingContent = false,
                 bool /*clearExtraSpace*/ = false, bool /*avoidReallocating*/ = false) {
        if (keepExistingContent) {
            std::vector<std::vector<T>> old = std::move(data_);
            data_.resize((size_t)numChannels);
            for (int ch = 0; ch < numChannels; ++ch) {
                data_[ch].resize((size_t)numSamples, T(0));
                if (ch < (int)old.size()) {
                    auto toCopy = std::min((int)old[ch].size(), numSamples);
                    std::copy(old[ch].begin(), old[ch].begin() + toCopy, data_[ch].begin());
                }
            }
        } else {
            data_.assign((size_t)numChannels, std::vector<T>((size_t)numSamples, T(0)));
        }
        numChannels_ = numChannels;
        numSamples_  = numSamples;
        rebuildPtrs();
    }

    int getNumChannels() const noexcept { return numChannels_; }
    int getNumSamples() const noexcept { return numSamples_; }

    const T* getReadPointer(int ch) const noexcept { return data_[(size_t)ch].data(); }
    T*       getWritePointer(int ch) noexcept { return data_[(size_t)ch].data(); }

    T** getArrayOfWritePointers() noexcept {
        rebuildPtrs();
        return ptrCache_.data();
    }

    void clear() {
        for (auto& ch : data_)
            std::fill(ch.begin(), ch.end(), T(0));
    }

    void clear(int ch, int startSample, int numSamples) {
        auto* p = getWritePointer(ch) + startSample;
        std::fill(p, p + numSamples, T(0));
    }

    void makeCopyOf(const AudioBuffer& other, bool /*avoidReallocating*/ = false) {
        setSize(other.numChannels_, other.numSamples_);
        for (int ch = 0; ch < numChannels_; ++ch)
            std::copy(other.data_[(size_t)ch].begin(), other.data_[(size_t)ch].end(),
                      data_[(size_t)ch].begin());
    }

    void copyFrom(int destCh, int destStart, const T* src, int numSamples) {
        std::copy(src, src + numSamples, getWritePointer(destCh) + destStart);
    }

    void addFrom(int destCh, int destStart, const T* src, int numSamples) {
        auto* dst = getWritePointer(destCh) + destStart;
        for (int i = 0; i < numSamples; ++i)
            dst[i] += src[i];
    }

    // All-channel gain from startSample
    void applyGain(int startSample, int numSamples, T gain) {
        for (int ch = 0; ch < numChannels_; ++ch) {
            auto* p = getWritePointer(ch) + startSample;
            for (int i = 0; i < numSamples; ++i)
                p[i] *= gain;
        }
    }

    // Per-channel gain
    void applyGain(int ch, int startSample, int numSamples, T gain) {
        auto* p = getWritePointer(ch) + startSample;
        for (int i = 0; i < numSamples; ++i)
            p[i] *= gain;
    }

    void applyGainRamp(int startSample, int numSamples, T startGain, T endGain) {
        if (numSamples <= 0)
            return;
        for (int ch = 0; ch < numChannels_; ++ch) {
            auto* p = getWritePointer(ch) + startSample;
            for (int i = 0; i < numSamples; ++i) {
                T g = startGain + (endGain - startGain) * T(i) / T(numSamples - 1);
                p[i] *= g;
            }
        }
    }

    void addFromWithRamp(int destCh, int destStart, const T* src, int numSamples, T startGain,
                         T endGain) {
        if (numSamples <= 0)
            return;
        auto* dst = getWritePointer(destCh) + destStart;
        for (int i = 0; i < numSamples; ++i) {
            T g = startGain + (endGain - startGain) * T(i) / T(numSamples - 1);
            dst[i] += src[i] * g;
        }
    }

  private:
    std::vector<std::vector<T>> data_;
    std::vector<T*>             ptrCache_;
    int                         numChannels_ = 0;
    int                         numSamples_  = 0;

    void rebuildPtrs() {
        ptrCache_.resize((size_t)numChannels_);
        for (int ch = 0; ch < numChannels_; ++ch)
            ptrCache_[(size_t)ch] = data_[(size_t)ch].data();
    }
};

// ---------------------------------------------------------------------------
// ScopedNoDenormals — flush-to-zero hint; no-op here
// ---------------------------------------------------------------------------
struct ScopedNoDenormals {};

// ---------------------------------------------------------------------------
// SmoothedValue
// ---------------------------------------------------------------------------
namespace ValueSmoothingTypes {
struct Linear {};
struct Multiplicative {};
} // namespace ValueSmoothingTypes

template <typename T, typename SmoothingType = ValueSmoothingTypes::Linear> class SmoothedValue {
  public:
    SmoothedValue() = default;
    explicit SmoothedValue(T initialValue) : current_(initialValue), target_(initialValue) {}

    void reset(double sampleRate, double rampLengthInSeconds) {
        numSteps_    = static_cast<int>(rampLengthInSeconds * sampleRate);
        currentStep_ = 0;
        start_       = current_;
    }

    void setCurrentAndTargetValue(T v) {
        current_ = target_ = v;
        currentStep_       = numSteps_;
    }

    void setTargetValue(T newTarget) {
        if (newTarget == target_)
            return;
        target_      = newTarget;
        currentStep_ = 0;
        start_       = current_;
        if constexpr (std::is_same_v<SmoothingType, ValueSmoothingTypes::Multiplicative>) {
            if (numSteps_ > 0 && start_ > T(0) && target_ > T(0))
                multiplier_ = std::pow(target_ / start_, T(1) / T(numSteps_));
        }
    }

    T getNextValue() noexcept {
        if (currentStep_ >= numSteps_)
            return current_ = target_;

        ++currentStep_;
        if constexpr (std::is_same_v<SmoothingType, ValueSmoothingTypes::Multiplicative>)
            current_ *= multiplier_;
        else
            current_ = start_ + (target_ - start_) * T(currentStep_) / T(numSteps_);
        return current_;
    }

    // Advance by count steps and return value at the end
    T skip(int count) noexcept {
        int end      = std::min(currentStep_ + count, numSteps_);
        currentStep_ = end;
        if (currentStep_ >= numSteps_)
            return current_ = target_;
        if constexpr (std::is_same_v<SmoothingType, ValueSmoothingTypes::Multiplicative>)
            current_ = start_ * std::pow(multiplier_, T(currentStep_));
        else
            current_ = start_ + (target_ - start_) * T(currentStep_) / T(numSteps_);
        return current_;
    }

    T    getCurrentValue() const noexcept { return current_; }
    T    getTargetValue() const noexcept { return target_; }
    bool isSmoothing() const noexcept { return currentStep_ < numSteps_; }

  private:
    T   current_     = T(0);
    T   target_      = T(0);
    T   start_       = T(0);
    T   multiplier_  = T(1);
    int numSteps_    = 0;
    int currentStep_ = 0;
};

// ---------------------------------------------------------------------------
// Random
// ---------------------------------------------------------------------------
class Random {
  public:
    Random() : rng_(std::random_device{}()) {}
    explicit Random(int64 seed) : rng_((uint64_t)seed) {}

    void setSeed(int64 seed) { rng_.seed((uint64_t)seed); }

    float nextFloat() noexcept { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_); }

    int nextInt(int upper) noexcept {
        return (int)(std::uniform_int_distribution<int>(0, upper - 1)(rng_));
    }

  private:
    std::mt19937_64 rng_;
};

// ---------------------------------------------------------------------------
// Array<T> — thin std::vector wrapper matching the JUCE API surface used
// ---------------------------------------------------------------------------
template <typename T> class Array {
  public:
    Array() = default;

    void resize(int newSize) { data_.assign((size_t)newSize, T{}); }

    T*       getRawDataPointer() noexcept { return data_.data(); }
    const T* getRawDataPointer() const noexcept { return data_.data(); }

    T&       operator[](int i) noexcept { return data_[(size_t)i]; }
    const T& operator[](int i) const noexcept { return data_[(size_t)i]; }

    int size() const noexcept { return (int)data_.size(); }

  private:
    std::vector<T> data_;
};

// ---------------------------------------------------------------------------
// namespace dsp
// ---------------------------------------------------------------------------
namespace dsp {

struct ProcessSpec {
    double   sampleRate;
    uint32_t maximumBlockSize;
    uint32_t numChannels;
};

// AudioBlock<T> — wraps an AudioBuffer or raw pointers
template <typename T> class AudioBlock {
  public:
    AudioBlock() = default;

    explicit AudioBlock(AudioBuffer<T>& buf)
        : numChannels_((size_t)buf.getNumChannels()), numSamples_((size_t)buf.getNumSamples()) {
        ptrs_.resize(numChannels_);
        for (size_t ch = 0; ch < numChannels_; ++ch)
            ptrs_[ch] = buf.getWritePointer((int)ch);
    }

    T*       getChannelPointer(size_t ch) noexcept { return ptrs_[ch]; }
    const T* getChannelPointer(size_t ch) const noexcept { return ptrs_[ch]; }

    size_t getNumSamples() const noexcept { return numSamples_; }
    size_t getNumChannels() const noexcept { return numChannels_; }

    void fill(T value) noexcept {
        for (size_t ch = 0; ch < numChannels_; ++ch)
            std::fill(ptrs_[ch], ptrs_[ch] + numSamples_, value);
    }

    AudioBlock& operator+=(const AudioBlock& other) noexcept {
        auto n = std::min(numSamples_, other.numSamples_);
        auto c = std::min(numChannels_, other.numChannels_);
        for (size_t ch = 0; ch < c; ++ch)
            for (size_t i = 0; i < n; ++i)
                ptrs_[ch][i] += other.ptrs_[ch][i];
        return *this;
    }

  private:
    std::vector<T*> ptrs_;
    size_t          numChannels_ = 0;
    size_t          numSamples_  = 0;
};

template <typename T> struct ProcessContextReplacing {
    explicit ProcessContextReplacing(AudioBlock<T>& block) : block_(block) {}

    AudioBlock<T>& getInputBlock() { return block_; }
    AudioBlock<T>& getOutputBlock() { return block_; }

    bool isBypassed = false;

  private:
    AudioBlock<T>& block_;
};

// ---------------------------------------------------------------------------
// Gain<T>
// ---------------------------------------------------------------------------
template <typename T> class Gain {
  public:
    void setGainLinear(T g) noexcept { gain_ = g; }
    T    getGainLinear() const noexcept { return gain_; }
    void prepare(const ProcessSpec&) noexcept {}
    void reset() noexcept {}

    T processSample(T /*dummy*/) const noexcept { return gain_; }

    template <typename Context> void process(const Context& ctx) noexcept {
        auto& block = const_cast<Context&>(ctx).getOutputBlock();
        for (size_t ch = 0; ch < block.getNumChannels(); ++ch) {
            auto* p = block.getChannelPointer(ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i)
                p[i] *= gain_;
        }
    }

  private:
    T gain_ = T(1);
};

// ---------------------------------------------------------------------------
// IIR biquad filter — direct form II transposed
// Coefficients stored as {b0, b1, b2, a1, a2} (a0 normalised to 1).
// ---------------------------------------------------------------------------
namespace IIR {
    template <typename T> struct Coefficients {
        using Ptr = std::shared_ptr<Coefficients>;
        std::array<T, 5> c{}; // b0 b1 b2 a1 a2

        static Ptr makePeakFilter(double sampleRate, double centreFreq, double Q,
                                  float gainLinear) {
            auto   coefs = std::make_shared<Coefficients>();
            double A     = (double)std::sqrt(gainLinear);
            double w0    = MathConstants<double>::twoPi * centreFreq / sampleRate;
            double cw = std::cos(w0), sw = std::sin(w0);
            double alpha = sw / (2.0 * Q);
            double b0 = 1.0 + alpha * A, b1 = -2.0 * cw, b2 = 1.0 - alpha * A;
            double a0 = 1.0 + alpha / A, a1 = -2.0 * cw, a2 = 1.0 - alpha / A;
            coefs->c = {T(b0 / a0), T(b1 / a0), T(b2 / a0), T(a1 / a0), T(a2 / a0)};
            return coefs;
        }

        static Ptr makeLowPass(double sampleRate, double cutoff, double Q = std::sqrt(0.5)) {
            auto   coefs = std::make_shared<Coefficients>();
            double w0    = MathConstants<double>::twoPi * cutoff / sampleRate;
            double cw = std::cos(w0), sw = std::sin(w0);
            double alpha = sw / (2.0 * Q);
            double a0    = 1.0 + alpha;
            coefs->c     = {T((1.0 - cw) * 0.5 / a0), T((1.0 - cw) / a0), T((1.0 - cw) * 0.5 / a0),
                            T(-2.0 * cw / a0), T((1.0 - alpha) / a0)};
            return coefs;
        }

        static Ptr makeHighPass(double sampleRate, double cutoff, double Q = std::sqrt(0.5)) {
            auto   coefs = std::make_shared<Coefficients>();
            double w0    = MathConstants<double>::twoPi * cutoff / sampleRate;
            double cw = std::cos(w0), sw = std::sin(w0);
            double alpha = sw / (2.0 * Q);
            double a0    = 1.0 + alpha;
            coefs->c     = {T((1.0 + cw) * 0.5 / a0), T(-(1.0 + cw) / a0), T((1.0 + cw) * 0.5 / a0),
                            T(-2.0 * cw / a0), T((1.0 - alpha) / a0)};
            return coefs;
        }
    };

    template <typename T> class Filter {
      public:
        typename Coefficients<T>::Ptr coefficients;

        void prepare(const ProcessSpec& spec) noexcept {
            s1_.assign(spec.numChannels, T(0));
            s2_.assign(spec.numChannels, T(0));
        }

        void reset() noexcept {
            std::fill(s1_.begin(), s1_.end(), T(0));
            std::fill(s2_.begin(), s2_.end(), T(0));
        }

        T processSample(int ch, T x) noexcept {
            const auto& c   = coefficients->c;
            T           y   = c[0] * x + s1_[(size_t)ch];
            s1_[(size_t)ch] = c[1] * x - c[3] * y + s2_[(size_t)ch];
            s2_[(size_t)ch] = c[2] * x - c[4] * y;
            return y;
        }

      private:
        std::vector<T> s1_, s2_;
    };
} // namespace IIR

// ProcessorDuplicator<Filter, Coefficients> — one filter per channel
template <typename FilterType, typename CoeffType> class ProcessorDuplicator {
  public:
    typename CoeffType::Ptr state; // public, like JUCE

    void prepare(const ProcessSpec& spec) {
        filters_.resize(spec.numChannels);
        for (auto& f : filters_) {
            f.coefficients = state;
            f.prepare(spec);
        }
    }

    void reset() {
        for (auto& f : filters_)
            f.reset();
    }

    template <typename Context> void process(Context&& ctx) {
        auto& block = ctx.getOutputBlock();
        for (size_t ch = 0; ch < block.getNumChannels() && ch < filters_.size(); ++ch) {
            filters_[ch].coefficients = state;
            auto* p                   = block.getChannelPointer(ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i)
                p[i] = filters_[ch].processSample((int)ch, p[i]);
        }
    }

  private:
    std::vector<FilterType> filters_;
};

// ---------------------------------------------------------------------------
// Oversampling<T> — simple 2× linear interpolation upsample / averaging downsample
// ---------------------------------------------------------------------------
template <typename T> class Oversampling {
  public:
    // Match ChowTape's constructor signature
    static constexpr int filterHalfBandPolyphaseIIR = 0;

    explicit Oversampling(int /*numChannels*/, int /*order*/ = 1, int /*filterType*/ = 0,
                          bool /*isMaxPhase*/ = true, bool /*useIntegerLatency*/ = false) {}

    void initProcessing(size_t maxBlockSize) { osBuffer_.setSize(1, (int)maxBlockSize * 2); }

    void reset() { osBuffer_.clear(); }

    float  getLatencyInSamples() const noexcept { return 0.0f; }
    size_t getOversamplingFactor() const noexcept { return 2; }

    AudioBlock<T> processSamplesUp(AudioBlock<T>& inputBlock) {
        auto n  = inputBlock.getNumSamples();
        auto ch = inputBlock.getNumChannels();
        osBuffer_.setSize((int)ch, (int)n * 2);
        for (size_t c = 0; c < ch; ++c) {
            const auto* src = inputBlock.getChannelPointer(c);
            auto*       dst = osBuffer_.getWritePointer((int)c);
            for (size_t i = 0; i < n; ++i) {
                dst[2 * i]     = src[i];
                dst[2 * i + 1] = (i + 1 < n) ? T(0.5) * (src[i] + src[i + 1]) : src[i];
            }
        }
        osBlock_ = AudioBlock<T>(osBuffer_);
        return osBlock_;
    }

    void processSamplesDown(AudioBlock<T>& outputBlock) {
        auto n  = outputBlock.getNumSamples();
        auto ch = outputBlock.getNumChannels();
        for (size_t c = 0; c < ch; ++c) {
            const auto* src = osBuffer_.getReadPointer((int)c);
            auto*       dst = outputBlock.getChannelPointer(c);
            for (size_t i = 0; i < n; ++i)
                dst[i] = T(0.5) * (src[2 * i] + src[2 * i + 1]);
        }
    }

  private:
    AudioBuffer<T> osBuffer_;
    AudioBlock<T>  osBlock_;
};

// ---------------------------------------------------------------------------
// FastMathApproximations — fall back to std
// ---------------------------------------------------------------------------
namespace FastMathApproximations {
    template <typename T> static T tan(T x) noexcept {
        return std::tan(x);
    }
} // namespace FastMathApproximations

// ---------------------------------------------------------------------------
// util::snapToZero — flush denormals; no-op scalar shim
// ---------------------------------------------------------------------------
namespace util {
    template <typename T> inline void snapToZero(T& /*x*/) noexcept {
    }
} // namespace util

} // namespace dsp

// Make juce::dsp::util::snapToZero accessible via its full qualified form
namespace juce {
namespace dsp {
    namespace util {
        template <typename T> inline void snapToZero(T& x) noexcept {
            ::dsp::util::snapToZero(x);
        }
    } // namespace util
} // namespace dsp
} // namespace juce

// ---------------------------------------------------------------------------
// FloatVectorOperations
// ---------------------------------------------------------------------------
struct FloatVectorOperations {
    static void multiply(float* dst, float multiplier, int numValues) noexcept {
        for (int i = 0; i < numValues; ++i)
            dst[i] *= multiplier;
    }

    static void multiply(float* dst, const float* src, int numValues) noexcept {
        for (int i = 0; i < numValues; ++i)
            dst[i] *= src[i];
    }

    static void copy(float* dst, const float* src, int numValues) noexcept {
        std::memcpy(dst, src, (size_t)numValues * sizeof(float));
    }

    static void abs(float* dst, const float* src, int numValues) noexcept {
        for (int i = 0; i < numValues; ++i)
            dst[i] = std::abs(src[i]);
    }
};
