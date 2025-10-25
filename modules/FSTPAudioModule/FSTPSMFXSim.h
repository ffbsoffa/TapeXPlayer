#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

class FSTPSMFXSim {
public:
    FSTPSMFXSim();

    void SetSampleRate(double sampleRate);
    void Reset();

    void Generate(float speed, double& outLeft, double& outRight);

private:
    static constexpr float kMinSpeed = 1.0f;
    static constexpr float kMaxSpeed = 32.0f;
    static constexpr float kBaseSpeed = 10.0f;
    static constexpr float kHighToneDb = -74.0f;
    static constexpr float kTriangleToneDb = -48.0f;
    static constexpr float kSineToneDb = -58.0f;
    static constexpr float kPinkNoiseDb = -57.0f;
    static constexpr float kLowBlockAttenuationDb = -3.0f;
    static constexpr float kOutputBoostDb = 6.0f;
    static constexpr double kPinkLowpassCutoffHz = 6000.0;
    static constexpr double kHighLowpassCutoffHz = 10000.0;
    static constexpr double kTwoPi = 6.283185307179586476925286766559;

    struct PinkNoiseFilterState {
        double b0 = 0.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double b3 = 0.0;
        double b4 = 0.0;
        double b5 = 0.0;
        double b6 = 0.0;
        double lowpassState1 = 0.0;
        double lowpassState2 = 0.0;
        uint32_t seed = 0;
    };

    static double WrapPhase(double phase);
    static double TriangleWave(double phase);
    static float DbToLinear(float db);
    static float SpeedToGainAdjustmentDb(float speed);

    float NextWhiteNoise(PinkNoiseFilterState& state);
    float NextPinkNoise(PinkNoiseFilterState& state);
    void UpdateCoefficients();
    double ApplyDualLowpass(double input, double alpha, double& s1, double& s2);

    double m_sampleRate = 48000.0;
    double m_invSampleRate = 1.0 / 48000.0;
    double m_pinkLpAlpha = 0.0;
    double m_highLpAlpha = 0.0;

    double m_sawPhase = 0.0;
    double m_squarePhase = 0.0;
    double m_trianglePhase = 0.0;
    double m_triangleSubPhase = 0.0;
    double m_sinePhase = 0.0;
    double m_sineSubPhase = 0.0;
    double m_highLpState1 = 0.0;
    double m_highLpState2 = 0.0;

    PinkNoiseFilterState m_pinkLeft;
    PinkNoiseFilterState m_pinkRight;
};
