#ifndef LOSSFILTER_H_INCLUDED
#define LOSSFILTER_H_INCLUDED

#include "../BypassProcessor.h"
#include "AzimuthProc.h"

class LossFilter {
  public:
    explicit LossFilter(int order = 64);
    ~LossFilter() {}

    void setParams(bool onOff, float speed, float spacing, float thickness, float gap,
                   float azimuth) {
        onOff_     = onOff;
        speed_     = speed;
        spacing_   = spacing;
        thickness_ = thickness;
        gap_       = gap;
        azimuth_   = azimuth;
    }

    void  prepare(float sampleRate, int samplesPerBlock, int numChannels);
    void  processBlock(AudioBuffer<float>& buffer);
    float getLatencySamples() const noexcept;

  private:
    using MultiChannelIIR =
        dsp::ProcessorDuplicator<dsp::IIR::Filter<float>, dsp::IIR::Coefficients<float>>;

    void        calcCoefs(MultiChannelIIR& filter);
    static void calcHeadBumpFilter(float speedIps, float gapMeters, double fs,
                                   MultiChannelIIR& filter);

    std::array<chowdsp::FIRFilter<float>, 2> filters;
    MultiChannelIIR                          bumpFilter[2];
    int                                      activeFilter = 0;
    int                                      fadeCount    = 0;
    int                                      fadeLength   = 1024;
    AudioBuffer<float>                       fadeBuffer;

    bool  onOff_     = true;
    float speed_     = 30.0f;
    float spacing_   = 0.1f;
    float thickness_ = 0.1f;
    float gap_       = 1.0f;
    float azimuth_   = 0.0f;

    float prevSpeed     = 0.5f;
    float prevSpacing   = 0.5f;
    float prevThickness = 0.5f;
    float prevGap       = 0.5f;

    float fs       = 44100.0f;
    float fsFactor = 1.0f;
    float binWidth = fs / 100.0f;

    const int    order;
    int          curOrder = order;
    Array<float> currentCoefs;
    Array<float> Hcoefs;

    AzimuthProc     azimuthProc;
    BypassProcessor bypass;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LossFilter)
};

#endif // LOSSFILTER_H_INCLUDED
