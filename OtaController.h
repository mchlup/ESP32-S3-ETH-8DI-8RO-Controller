#pragma once

// ---------------------------------------------------------------------------
// Optional feature
//
// FEATURE_OTA is defined by FeatureOTA.h (included from Features.h).
// When disabled, this header provides lightweight no-op stubs.
// ---------------------------------------------------------------------------

#if defined(FEATURE_OTA)

namespace OTA {
  void init();
  void loop();
}

#else

namespace OTA {
  inline void init() {}
  inline void loop() {}
}

#endif
