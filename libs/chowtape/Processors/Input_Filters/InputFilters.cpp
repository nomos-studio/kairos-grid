#include "InputFilters.h"

void InputFilters::prepareToPlay(double sampleRate, int samplesPerBlock, int numChannels) {
    fs = (float)sampleRate;
    dsp::ProcessSpec spec{sampleRate, (uint32)samplesPerBlock, (uint32)numChannels};
    lowCutFilter.prepare(spec);
    highCutFilter.prepare(spec);
    makeupDelay.prepare(spec);

    lowCutBuffer.setSize(numChannels, samplesPerBlock);
    highCutBuffer.setSize(numChannels, samplesPerBlock);
    makeupBuffer.setSize(numChannels, samplesPerBlock);

    bypass.prepare(samplesPerBlock, numChannels, onOff_);
    makeupBypass.prepare(samplesPerBlock, numChannels, onOff_);
}

void InputFilters::processBlock(AudioBuffer<float>& buffer) {
    if (!bypass.processBlockIn(buffer, onOff_))
        return;

    lowCutFilter.setCutoff(lowCut_);
    highCutFilter.setCutoff(jmin(highCut_, fs * 0.48f));

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        auto* data          = buffer.getWritePointer(ch);
        auto* cutLowSignal  = lowCutBuffer.getWritePointer(ch);
        auto* cutHighSignal = highCutBuffer.getWritePointer(ch);

        for (int n = 0; n < buffer.getNumSamples(); ++n)
            lowCutFilter.processSample((size_t)ch, data[n], cutLowSignal[n], data[n]);

        for (int n = 0; n < buffer.getNumSamples(); ++n)
            highCutFilter.processSample((size_t)ch, data[n], data[n], cutHighSignal[n]);
    }

    bypass.processBlockOut(buffer, onOff_);
    // snapToZero is a no-op in our compat shim
}

void InputFilters::processBlockMakeup(AudioBuffer<float>& buffer) {
    if (!makeupBypass.processBlockIn(buffer, onOff_))
        return;

    if (!makeup_) {
        makeupBypass.processBlockOut(buffer, onOff_);
        return;
    }

    makeupBuffer.setSize(buffer.getNumChannels(), buffer.getNumSamples(), false, false, true);
    dsp::AudioBlock<float> lowCutBlock(lowCutBuffer);
    dsp::AudioBlock<float> highCutBlock(highCutBuffer);
    dsp::AudioBlock<float> makeupBlock(makeupBuffer);

    makeupBlock.fill(0.0f);
    makeupBlock += lowCutBlock;
    makeupBlock += highCutBlock;

    dsp::ProcessContextReplacing<float> context(makeupBlock);
    makeupDelay.process(context);

    dsp::AudioBlock<float> outputBlock(buffer);
    outputBlock += makeupBlock;

    makeupBypass.processBlockOut(buffer, onOff_);
}
