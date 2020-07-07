// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/media/tuning/cpp/fidl.h>
#include <fuchsia/virtualaudio/cpp/fidl.h>

#include <cmath>

#include "src/media/audio/lib/test/hermetic_audio_test.h"

namespace media::audio::test {

class AudioTunerTest : public HermeticAudioTest {
 protected:
  void TearDown() override {
    audio_renderer_.Unbind();
    audio_capturer_.Unbind();

    HermeticAudioTest::TearDown();
  }

  fuchsia::media::AudioRendererPtr audio_renderer_;
  fuchsia::media::AudioCapturerPtr audio_capturer_;
};

// Test that the user is connected to the audio tuner.
// TODO(52962): Flesh out
TEST_F(AudioTunerTest, ConnectToAudioTuner) {
  fuchsia::media::tuning::AudioTunerPtr audio_tuner;
  environment()->ConnectToService(audio_tuner.NewRequest());
  audio_tuner.set_error_handler(ErrorHandler());
  audio_tuner->GetAvailableAudioEffects(
      CompletionCallback([](std::vector<fuchsia::media::tuning::AudioEffectType>) {}));
  ExpectCallback();
}

}  // namespace media::audio::test