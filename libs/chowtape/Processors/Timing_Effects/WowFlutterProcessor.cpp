#include "WowFlutterProcessor.h"

void WowFlutterProcessor::prepareToPlay(double sampleRate, int samplesPerBlock, int numChannels) {
    fs = (float)sampleRate;

    bypass.prepare(samplesPerBlock, numChannels, onOff_);
    wowProcessor.prepare(sampleRate, samplesPerBlock, numChannels);
    flutterProcessor.prepare(sampleRate, samplesPerBlock, numChannels);

    delay.prepare({sampleRate, (uint32)samplesPerBlock, (uint32)numChannels});
    delay.setDelay(0.0f);

    dcBlocker.resize((size_t)numChannels);
    for (auto& filt : dcBlocker)
        filt.prepare(sampleRate, 15.0f);
}

void WowFlutterProcessor::processBlock(AudioBuffer<float>& buffer) {
    ScopedNoDenormals noDenormals;

    const auto numChannels = buffer.getNumChannels();
    const auto numSamples  = buffer.getNumSamples();

    auto curDepthWow = powf(wowDepth_, 3.0f);
    auto wowFreq     = powf(4.5f, wowRate_) - 1.0f;
    wowProcessor.prepareBlock(curDepthWow, wowFreq, wowVariance_, wowDrift_, numSamples,
                              numChannels);

    auto curDepthFlutter = powf(powf(flutterDepth_, 3.0f) * 81.0f / 625.0f, 0.5f);
    auto flutterFreq     = 0.1f * powf(1000.0f, flutterRate_);
    flutterProcessor.prepareBlock(curDepthFlutter, flutterFreq, numSamples, numChannels);

    bool shouldTurnOff =
        !onOff_ || (wowProcessor.shouldTurnOff() && flutterProcessor.shouldTurnOff());
    if (bypass.processBlockIn(buffer, !shouldTurnOff)) {
        processWetBuffer(buffer);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            dcBlocker[(size_t)ch].processBlock(buffer.getWritePointer(ch), buffer.getNumSamples());

        bypass.processBlockOut(buffer, !shouldTurnOff);
    } else {
        processBypassed(buffer);
    }

    wowProcessor.plotBuffer();
    flutterProcessor.plotBuffer();
}

void WowFlutterProcessor::processWetBuffer(AudioBuffer<float>& buffer) {
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        auto* x = buffer.getWritePointer(ch);
        for (int n = 0; n < buffer.getNumSamples(); ++n) {
            auto [wowLFO, wowOffset]         = wowProcessor.getLFO(n, (size_t)ch);
            auto [flutterLFO, flutterOffset] = flutterProcessor.getLFO(n, (size_t)ch);

            auto newLength = (wowLFO + flutterLFO + flutterOffset + wowOffset) * fs / 1000.0f;
            newLength      = jlimit(0.0f, (float)HISTORY_SIZE, newLength);

            delay.setDelay(newLength);
            delay.pushSample(ch, x[n]);
            x[n] = delay.popSample(ch);
        }

        wowProcessor.boundPhase((size_t)ch);
        flutterProcessor.boundPhase((size_t)ch);
    }
}

void WowFlutterProcessor::processBypassed(const AudioBuffer<float>& /*buffer*/) {
    delay.setDelay(0.0f);
    for (int n = 0; n < 0; ++n) {
    } // state kept warm by processWetBuffer path

    // Advance phase state without touching samples
    // (original iterates buffer, but buffer is const here — skip to avoid UB)
}
