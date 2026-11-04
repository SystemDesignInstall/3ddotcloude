# Dependencies and references

- Google ARCore `com.google.ar:core:1.57.0`: binary SDK governed by [ARCore Additional Terms of Service](https://developers.google.com/ar/develop/terms). The on-device Google Play Services for AR runtime is separately installed. [Upstream LICENSE](https://github.com/google-ar/arcore-android-sdk/blob/main/LICENSE) distinguishes binaries and Apache-2.0 example source.
- Android Gradle Plugin 8.7.3: Apache-2.0 build tooling; Gradle 8.9 wrapper/tooling: Apache-2.0. Android SDK packages are installed separately under their own SDK terms.
- Implementation reference: [official raw_depth_java sample](https://github.com/google-ar/arcore-android-sdk/tree/main/samples/raw_depth_java), especially depth unprojection and camera texture/image coordinate mapping. The app implementation is written for this repository; no Tango3DR, 3DLiveScanner, RTAB-Map, Open3D, COLMAP or AI model is bundled in the APK.
- Application discloses ARCore use and links to Google's privacy policy. No network-upload feature is implemented by Spatial Capture. Other installed services follow their own policies.

This is a development preview, not a cleared commercial release. Binary dependency and SDK terms must remain attached to the release review; the root registry tracks this isolated module as planned for production.
