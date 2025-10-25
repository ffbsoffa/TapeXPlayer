#include "FSTPSMFXSim.h"

FSTPSMFXSim::FSTPSMFXSim() {
    m_pinkLeft.seed = 0x12345678u;
    m_pinkRight.seed = 0x87654321u;
    UpdateCoefficients();
}

void FSTPSMFXSim::SetSampleRate(double sampleRate) {
    if (sampleRate <= 0.0) {
        sampleRate = 48000.0;
    }
    m_sampleRate = sampleRate;
    m_invSampleRate = 1.0 / m_sampleRate;
    UpdateCoefficients();
}

void FSTPSMFXSim::Reset() {
    m_sawPhase = 0.0;
    m_squarePhase = 0.0;
    m_trianglePhase = 0.0;
    m_triangleSubPhase = 0.0;
    m_sinePhase = 0.0;
    m_sineSubPhase = 0.0;
    m_highLpState1 = 0.0;
    m_highLpState2 = 0.0;

    m_pinkLeft = PinkNoiseFilterState{};
    m_pinkRight = PinkNoiseFilterState{};
    m_pinkLeft.seed = 0x12345678u;
    m_pinkRight.seed = 0x87654321u;
}

void FSTPSMFXSim::Generate(float rawSpeed, double& outLeft, double& outRight) {
    const float clampedSpeed = std::clamp(rawSpeed, 0.0f, kMaxSpeed);

    float baseRamp = 1.0f;
    if (clampedSpeed <= kBaseSpeed) {
        baseRamp = std::clamp((clampedSpeed - kMinSpeed) / (kBaseSpeed - kMinSpeed), 0.0f, 1.0f);
    }

    if (baseRamp <= 0.0001f) {
        // Still advance phases to avoid discontinuities, but output silence
        const double baseFrequencySilent = std::max(80.0, 100.0 * static_cast<double>(std::max(clampedSpeed, kMinSpeed)));
        const double invSampleRateSilent = m_invSampleRate;
        m_sawPhase = WrapPhase(m_sawPhase + baseFrequencySilent * invSampleRateSilent);
        m_squarePhase = WrapPhase(m_squarePhase + baseFrequencySilent * invSampleRateSilent);
        m_trianglePhase = WrapPhase(m_trianglePhase + (baseFrequencySilent * 0.05) * invSampleRateSilent);
        m_triangleSubPhase = WrapPhase(m_triangleSubPhase + (baseFrequencySilent * 0.025) * invSampleRateSilent);
        m_sinePhase = WrapPhase(m_sinePhase + (baseFrequencySilent * 0.05 - 1.0) * invSampleRateSilent);
        m_sineSubPhase = WrapPhase(m_sineSubPhase + (baseFrequencySilent * 0.025 - 1.0) * invSampleRateSilent);
        outLeft = 0.0;
        outRight = 0.0;
        return;
    }

    const float speed = std::clamp(std::max(clampedSpeed, kMinSpeed), kMinSpeed, kMaxSpeed);
    const double baseFrequency = std::max(80.0, 100.0 * static_cast<double>(speed));
    const double sawFrequency = baseFrequency;
    const double squareFrequency = std::max(50.0, baseFrequency - 3.0);
    const double triangleFrequency = std::max(10.0, baseFrequency * 0.05);
    const double triangleSubFrequency = std::max(5.0, triangleFrequency * 0.5);
    const double sineFrequency = std::max(9.0, triangleFrequency - 1.0);
    const double sineSubFrequency = std::max(4.0, triangleSubFrequency - 1.0);

    const float adjustmentDb = SpeedToGainAdjustmentDb(speed);

    const float triangleGainBase = DbToLinear(kTriangleToneDb + adjustmentDb + kLowBlockAttenuationDb);
    const float triangleSubGainBase = DbToLinear(kTriangleToneDb + adjustmentDb + kLowBlockAttenuationDb - 3.0f);
    const float sineGainBase = DbToLinear(kSineToneDb + adjustmentDb + kLowBlockAttenuationDb);
    const float sineSubGainBase = DbToLinear(kSineToneDb + adjustmentDb + kLowBlockAttenuationDb - 3.0f);

    float subRamp = 0.0f;
    if (speed > kBaseSpeed) {
        subRamp = std::clamp((speed - kBaseSpeed) / (kMaxSpeed - kBaseSpeed), 0.0f, 1.0f);
    }

    float pinkGain = 0.0f;
    if (speed <= kBaseSpeed) {
        pinkGain = DbToLinear(kPinkNoiseDb - 6.0f) * baseRamp;
    } else {
        const float highRamp = std::clamp((speed - kBaseSpeed) / (kMaxSpeed - kBaseSpeed), 0.0f, 1.0f);
        const float pinkDb = kPinkNoiseDb - 6.0f + 6.0f * highRamp;
        pinkGain = DbToLinear(pinkDb);
    }

    m_sawPhase = WrapPhase(m_sawPhase + sawFrequency * m_invSampleRate);
    m_squarePhase = WrapPhase(m_squarePhase + squareFrequency * m_invSampleRate);
    m_trianglePhase = WrapPhase(m_trianglePhase + triangleFrequency * m_invSampleRate);
    m_triangleSubPhase = WrapPhase(m_triangleSubPhase + triangleSubFrequency * m_invSampleRate);
    m_sinePhase = WrapPhase(m_sinePhase + sineFrequency * m_invSampleRate);
    m_sineSubPhase = WrapPhase(m_sineSubPhase + sineSubFrequency * m_invSampleRate);

    const double sawRaw = (m_sawPhase * 2.0) - 1.0;
    const double squareRaw = (m_squarePhase < 0.5) ? 1.0 : -1.0;
    const double triangleMain = TriangleWave(m_trianglePhase);
    const double triangleSub = TriangleWave(m_triangleSubPhase);
    const double sineMain = std::sin(m_sinePhase * kTwoPi);
    const double sineSub = std::sin(m_sineSubPhase * kTwoPi);

    const double highCombined = (sawRaw + squareRaw) * static_cast<double>(DbToLinear(kHighToneDb));
    const double filteredHighRaw = ApplyDualLowpass(highCombined, m_highLpAlpha, m_highLpState1, m_highLpState2);
    const double filteredHigh = filteredHighRaw * static_cast<double>(baseRamp);

    const double triangleComponent = triangleMain * static_cast<double>(triangleGainBase) * static_cast<double>(baseRamp);
    const double triangleComponentSecondary = triangleSub * static_cast<double>(triangleSubGainBase) * static_cast<double>(baseRamp * subRamp);
    const double sineComponent = sineMain * static_cast<double>(sineGainBase) * static_cast<double>(baseRamp);
    const double sineComponentSecondary = sineSub * static_cast<double>(sineSubGainBase) * static_cast<double>(baseRamp * subRamp);

    const double noiseLeft = static_cast<double>(NextPinkNoise(m_pinkLeft)) * static_cast<double>(pinkGain);
    const double noiseRight = static_cast<double>(NextPinkNoise(m_pinkRight)) * static_cast<double>(pinkGain);

    const double lowSum = triangleComponent + triangleComponentSecondary + sineComponent + sineComponentSecondary;
    const double totalLeft = (filteredHigh + lowSum + noiseLeft) * static_cast<double>(DbToLinear(kOutputBoostDb));
    const double totalRight = (filteredHigh + lowSum + noiseRight) * static_cast<double>(DbToLinear(kOutputBoostDb));
    outLeft = std::clamp(totalLeft, -0.95, 0.95);
    outRight = std::clamp(totalRight, -0.95, 0.95);
}

double FSTPSMFXSim::WrapPhase(double phase) {
    phase -= std::floor(phase);
    return phase;
}

double FSTPSMFXSim::TriangleWave(double phase) {
    double value = phase * 4.0;
    if (value < 2.0) return value - 1.0;
    return 3.0 - value;
}

float FSTPSMFXSim::DbToLinear(float db) {
    return std::pow(10.0f, db * 0.05f);
}

float FSTPSMFXSim::SpeedToGainAdjustmentDb(float speed) {
    if (speed <= kBaseSpeed) {
        return 0.0f;
    }
    if (speed >= kMaxSpeed) {
        return 6.0f;
    }
    const float t = (speed - kBaseSpeed) / (kMaxSpeed - kBaseSpeed);
    return 6.0f * std::clamp(t, 0.0f, 1.0f);
}

float FSTPSMFXSim::NextWhiteNoise(PinkNoiseFilterState& state) {
    state.seed = state.seed * 1664525u + 1013904223u;
    constexpr double invUint32 = 1.0 / static_cast<double>(UINT32_MAX);
    const double normalized = static_cast<double>(state.seed) * invUint32;
    return static_cast<float>(normalized * 2.0 - 1.0);
}

float FSTPSMFXSim::NextPinkNoise(PinkNoiseFilterState& state) {
    const double white = static_cast<double>(NextWhiteNoise(state));

    state.b0 = 0.99886 * state.b0 + white * 0.0555179;
    state.b1 = 0.99332 * state.b1 + white * 0.0750759;
    state.b2 = 0.96900 * state.b2 + white * 0.1538520;
    state.b3 = 0.86650 * state.b3 + white * 0.3104856;
    state.b4 = 0.55000 * state.b4 + white * 0.5329522;
    state.b5 = -0.7616 * state.b5 - white * 0.0168980;

    double pink = state.b0 + state.b1 + state.b2 + state.b3 + state.b4 + state.b5 + state.b6 + white * 0.5362;
    state.b6 = white * 0.115926;

    pink = std::clamp(pink * 0.11, -1.0, 1.0);
    const double lp1 = (1.0 - m_pinkLpAlpha) * pink + m_pinkLpAlpha * state.lowpassState1;
    state.lowpassState1 = lp1;
    const double lp2 = (1.0 - m_pinkLpAlpha) * lp1 + m_pinkLpAlpha * state.lowpassState2;
    state.lowpassState2 = lp2;
    return static_cast<float>(std::clamp(lp2, -1.0, 1.0));
}

void FSTPSMFXSim::UpdateCoefficients() {
    m_pinkLpAlpha = std::exp(-kTwoPi * kPinkLowpassCutoffHz / m_sampleRate);
    m_highLpAlpha = std::exp(-kTwoPi * kHighLowpassCutoffHz / m_sampleRate);
}

double FSTPSMFXSim::ApplyDualLowpass(double input, double alpha, double& s1, double& s2) {
    const double filtered1 = (1.0 - alpha) * input + alpha * s1;
    s1 = filtered1;
    const double filtered2 = (1.0 - alpha) * filtered1 + alpha * s2;
    s2 = filtered2;
    return filtered2;
}
