#include "CompressionProcessor.h"

static float compressionDB(float xDB, float dbPlus) noexcept {
    if (dbPlus <= 0.0f)
        return dbPlus;
    float window = 2.0f * dbPlus;
    if (xDB < -window)
        return dbPlus;
    return std::log(xDB + window + 1.0f) - dbPlus - xDB;
}

void CompressionProcessor::prepare(double sr, int samplesPerBlock, int numChannels) {
    oversample = std::make_unique<dsp::Oversampling<float>>(
        numChannels, 1, dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    oversample->initProcessing((size_t)samplesPerBlock);
    auto osFactor = oversample->getOversamplingFactor();
    bypass.prepare(samplesPerBlock, numChannels, onOff_);

    slewLimiter.clear();
    dbPlusSmooth.clear();
    for (int ch = 0; ch < numChannels; ++ch) {
        slewLimiter.emplace_back();
        dbPlusSmooth.emplace_back();

        slewLimiter[(size_t)ch].prepare({sr, (uint32)samplesPerBlock, 1});
        dbPlusSmooth[(size_t)ch].reset(sr, 0.05);
    }

    xDBVec.resize(osFactor * (size_t)samplesPerBlock, 0.0f);
    compGainVec.resize(osFactor * (size_t)samplesPerBlock, 0.0f);
}

void CompressionProcessor::processBlock(AudioBuffer<float>& buffer) {
    if (!bypass.processBlockIn(buffer, onOff_))
        return;

    dsp::AudioBlock<float> block(buffer);
    auto                   osBlock = oversample->processSamplesUp(block);

    const auto numSamples = (int)osBlock.getNumSamples();
    xDBVec.resize((size_t)numSamples);
    compGainVec.resize((size_t)numSamples);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        dbPlusSmooth[(size_t)ch].setTargetValue(amount_);

        auto* x = osBlock.getChannelPointer((size_t)ch);

        for (int n = 0; n < numSamples; ++n)
            xDBVec[(size_t)n] = Decibels::gainToDecibels(std::abs(x[n]));

        for (int n = 0; n < numSamples; ++n) {
            float dbPlus = dbPlusSmooth[(size_t)ch].getNextValue();
            compGainVec[(size_t)n] =
                Decibels::decibelsToGain(compressionDB(xDBVec[(size_t)n], dbPlus));
        }

        // attack/release are intentionally swapped: slew limits gain recovery rate
        slewLimiter[(size_t)ch].setParameters(release_, attack_);
        for (int k = 0; k < numSamples; ++k)
            compGainVec[(size_t)k] =
                jmin(compGainVec[(size_t)k],
                     slewLimiter[(size_t)ch].processSample(compGainVec[(size_t)k]));

        FloatVectorOperations::multiply(x, compGainVec.data(), numSamples);
    }

    oversample->processSamplesDown(block);
    bypass.processBlockOut(buffer, onOff_);
}

float CompressionProcessor::getLatencySamples() const noexcept {
    if (oversample == nullptr)
        return 0.0f;
    return onOff_ ? oversample->getLatencyInSamples() : 0.0f;
}
