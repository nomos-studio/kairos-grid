#ifndef WOWFLUTTERPROCESSOR_H_INCLUDED
#define WOWFLUTTERPROCESSOR_H_INCLUDED

#include "../BypassProcessor.h"
#include "../Hysteresis/DCBlocker.h"
#include "FlutterProcess.h"
#include "WowProcess.h"

class WowFlutterProcessor {
  public:
    WowFlutterProcessor() = default;

    void setParams(bool onOff, float flutterRate, float flutterDepth, float wowRate, float wowDepth,
                   float wowVariance, float wowDrift) {
        onOff_        = onOff;
        flutterRate_  = flutterRate;
        flutterDepth_ = flutterDepth;
        wowRate_      = wowRate;
        wowDepth_     = wowDepth;
        wowVariance_  = wowVariance;
        wowDrift_     = wowDrift;
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock, int numChannels);
    void processBlock(AudioBuffer<float>&);

  private:
    void processWetBuffer(AudioBuffer<float>& buffer);
    void processBypassed(const AudioBuffer<float>& buffer);

    bool  onOff_        = true;
    float flutterRate_  = 0.3f;
    float flutterDepth_ = 0.0f;
    float wowRate_      = 0.25f;
    float wowDepth_     = 0.0f;
    float wowVariance_  = 0.0f;
    float wowDrift_     = 0.0f;

    BypassProcessor bypass;
    float           fs = 48000.0f;

    WowProcess     wowProcessor;
    FlutterProcess flutterProcessor;

    enum {
        HISTORY_SIZE = 1 << 21,
    };

    chowdsp::DelayLine<float, chowdsp::DelayLineInterpolationTypes::Lagrange3rd> delay{
        HISTORY_SIZE};
    std::vector<DCBlocker> dcBlocker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WowFlutterProcessor)
};

#endif // WOWFLUTTRPROCESSOR_H_INCLUDED
