#include "LossFilter.h"

LossFilter::LossFilter(int order) : order(order) {
}

float LossFilter::getLatencySamples() const noexcept {
    return onOff_ ? (float)curOrder / 2.0f : 0.0f;
}

void LossFilter::prepare(float sampleRate, int samplesPerBlock, int numChannels) {
    fs = sampleRate;
    fadeBuffer.setSize(numChannels, samplesPerBlock);
    fadeLength = jmax(1024, samplesPerBlock);

    fsFactor = (float)fs / 44100.0f;
    curOrder = int((float)order * fsFactor);
    currentCoefs.resize(curOrder);
    Hcoefs.resize(curOrder);

    bumpFilter[0].prepare({(double)sampleRate, (uint32)samplesPerBlock, (uint32)numChannels});
    bumpFilter[1].prepare({(double)sampleRate, (uint32)samplesPerBlock, (uint32)numChannels});
    calcCoefs(bumpFilter[activeFilter]);

    for (auto& filter : filters) {
        filter.setOrder(curOrder);
        filter.prepare(numChannels);
        filter.setCoefficients(currentCoefs.getRawDataPointer());
    }

    prevSpeed     = speed_;
    prevSpacing   = spacing_;
    prevThickness = thickness_;
    prevGap       = gap_;

    azimuthProc.prepare(sampleRate, samplesPerBlock);
    bypass.prepare(samplesPerBlock, numChannels, onOff_);
}

void LossFilter::calcHeadBumpFilter(float speedIps, float gapMeters, double fs,
                                    MultiChannelIIR& filter) {
    auto bumpFreq = speedIps * 0.0254f / (gapMeters * 500.0f);
    auto gain     = jmax(1.5f * (1000.0f - std::abs(bumpFreq - 100.0f)) / 1000.0f, 1.0f);
    *filter.state = *dsp::IIR::Coefficients<float>::makePeakFilter(fs, (double)bumpFreq, 2.0, gain);
}

void LossFilter::calcCoefs(MultiChannelIIR& filter) {
    binWidth = fs / (float)curOrder;
    auto H   = Hcoefs.getRawDataPointer();
    for (int k = 0; k < curOrder / 2; k++) {
        const auto freq = (float)k * binWidth;
        const auto waveNumber =
            MathConstants<float>::twoPi * jmax(freq, 20.0f) / (speed_ * 0.0254f);
        const auto thickTimesK = waveNumber * (thickness_ * (float)1.0e-6);
        const auto kGapOverTwo = waveNumber * (gap_ * (float)1.0e-6) / 2.0f;

        H[k] = expf(-waveNumber * (spacing_ * (float)1.0e-6));
        H[k] *= (1.0f - expf(-thickTimesK)) / thickTimesK;
        H[k] *= sinf(kGapOverTwo) / kGapOverTwo;
        H[curOrder - k - 1] = H[k];
    }

    auto h = currentCoefs.getRawDataPointer();
    for (int n = 0; n < curOrder / 2; n++) {
        const auto idx = (size_t)curOrder / 2 + (size_t)n;
        for (int k = 0; k < curOrder; k++)
            h[idx] += Hcoefs[k] *
                      cosf(MathConstants<float>::twoPi * (float)k * (float)n / (float)curOrder);

        h[idx] /= (float)curOrder;
        h[curOrder / 2 - n] = h[idx];
    }

    calcHeadBumpFilter(speed_, gap_ * (float)1.0e-6, (double)fs, filter);
}

void LossFilter::processBlock(AudioBuffer<float>& buffer) {
    const auto numChannels = buffer.getNumChannels();
    const auto numSamples  = buffer.getNumSamples();

    if (!bypass.processBlockIn(buffer, onOff_))
        return;

    if ((speed_ != prevSpeed || spacing_ != prevSpacing || thickness_ != prevThickness ||
         gap_ != prevGap) &&
        fadeCount == 0) {
        calcCoefs(bumpFilter[!activeFilter]);
        filters[!activeFilter].setCoefficients(currentCoefs.getRawDataPointer());

        bumpFilter[!activeFilter].reset();

        fadeCount     = fadeLength;
        prevSpeed     = speed_;
        prevSpacing   = spacing_;
        prevThickness = thickness_;
        prevGap       = gap_;
    }

    if (fadeCount > 0)
        fadeBuffer.makeCopyOf(buffer, true);
    else
        filters[!activeFilter].processBlockBypassed(buffer);

    {
        dsp::AudioBlock<float> block(buffer);
        filters[activeFilter].processBlock(buffer);
        bumpFilter[activeFilter].process(dsp::ProcessContextReplacing<float>{block});
    }

    if (fadeCount > 0) {
        dsp::AudioBlock<float> fadeBlock(fadeBuffer);
        filters[!activeFilter].processBlock(fadeBuffer);
        bumpFilter[!activeFilter].process(dsp::ProcessContextReplacing<float>{fadeBlock});

        auto startGain     = (float)fadeCount / (float)fadeLength;
        auto samplesToFade = jmin(fadeCount, numSamples);
        fadeCount -= samplesToFade;
        auto endGain = (float)fadeCount / (float)fadeLength;

        buffer.applyGainRamp(0, samplesToFade, startGain, endGain);
        buffer.applyGain(samplesToFade, numSamples - samplesToFade, endGain);

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.addFromWithRamp(ch, 0, fadeBuffer.getReadPointer(ch), samplesToFade,
                                   1.0f - startGain, 1.0f - endGain);

        if (fadeCount == 0)
            activeFilter = !activeFilter;
    }

    azimuthProc.setAzimuthAngle(azimuth_, speed_);
    azimuthProc.processBlock(buffer);

    bypass.processBlockOut(buffer, onOff_);
}
