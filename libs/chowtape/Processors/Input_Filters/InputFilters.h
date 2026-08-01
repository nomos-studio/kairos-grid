#ifndef INPUTFILTERS_H_INCLUDED
#define INPUTFILTERS_H_INCLUDED

#include "../BypassProcessor.h"
#include "LinkwitzRileyFilter.h"

class InputFilters {
  public:
    InputFilters() = default;

    void setParams(bool onOff, float lowCut, float highCut, bool makeup) {
        onOff_   = onOff;
        lowCut_  = lowCut;
        highCut_ = highCut;
        makeup_  = makeup;
    }

    void setMakeupDelay(float newDelaySamples) { makeupDelay.setDelay(newDelaySamples); }

    void prepareToPlay(double sampleRate, int samplesPerBlock, int numChannels);
    void processBlock(AudioBuffer<float>& buffer);
    void processBlockMakeup(AudioBuffer<float>& buffer);

  private:
    bool  onOff_   = false;
    float lowCut_  = 20.0f;
    float highCut_ = 22000.0f;
    bool  makeup_  = false;

    float                                                                fs = 44100.0f;
    LinkwitzRileyFilter<float>                                           lowCutFilter;
    LinkwitzRileyFilter<float>                                           highCutFilter;
    dsp::DelayLine<float, dsp::DelayLineInterpolationTypes::Lagrange3rd> makeupDelay{1 << 21};

    AudioBuffer<float> lowCutBuffer, highCutBuffer, makeupBuffer;
    BypassProcessor    bypass;
    BypassProcessor    makeupBypass;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputFilters)
};

#endif // !INPUTFILTERS_H_INCLUDED
