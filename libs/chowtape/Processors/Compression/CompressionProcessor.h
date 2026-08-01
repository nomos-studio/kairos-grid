#ifndef COMPRESSIONPROCESSOR_H_INCLUDED
#define COMPRESSIONPROCESSOR_H_INCLUDED

#include "../BypassProcessor.h"

class CompressionProcessor {
  public:
    CompressionProcessor() = default;

    void setParams(bool onOff, float amount, float attackMs, float releaseMs) {
        onOff_   = onOff;
        amount_  = amount;
        attack_  = attackMs;
        release_ = releaseMs;
    }

    void prepare(double sr, int samplesPerBlock, int numChannels);
    void processBlock(AudioBuffer<float>& buffer);

    float getLatencySamples() const noexcept;

  private:
    bool  onOff_   = false;
    float amount_  = 0.0f;
    float attack_  = 5.0f;
    float release_ = 200.0f;

    std::vector<chowdsp::LevelDetector<float>> slewLimiter;
    BypassProcessor                            bypass;

    std::unique_ptr<dsp::Oversampling<float>> oversample;

    std::vector<SmoothedValue<float, ValueSmoothingTypes::Linear>> dbPlusSmooth;

    std::vector<float> xDBVec;
    std::vector<float> compGainVec;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CompressionProcessor)
};

#endif // COMPRESSIONPROCESSOR_H_INCLUDED
