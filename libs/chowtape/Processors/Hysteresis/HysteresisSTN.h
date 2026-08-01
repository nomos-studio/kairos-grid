// SPDX-License-Identifier: LGPL-2.1-or-later
// Stub for HysteresisSTN — omits RTNeural dependency.
// STN solver case falls through to RK4 in HysteresisProcessing::process().
#pragma once

struct HysteresisSTN {
    static constexpr double diffMakeup = 1.0;

    void prepare(double /*sampleRate*/) {}
    void setParams(float /*sat*/, float /*width*/) {}

    double process(const double* /*input*/) { return 0.0; }
};
