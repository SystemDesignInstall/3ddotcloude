package com.systemdesign.capture;

/**
 * Pure capture-boundary math; meters, right-handed ARCore world (+Y up). Camera pixels use top-left
 * origin. ARCore camera is +X right, +Y up, -Z forward.
 */
public final class Geometry {
  private Geometry() {}

  public static void unproject(
      int x, int y, int millimeters, float[] k, float[] worldFromGlCamera, float[] out) {
    float z = millimeters * .001f;
    float px = (x - k[2]) * z / k[0], py = (k[3] - y) * z / k[1], pz = -z;
    for (int r = 0; r < 3; r++)
      out[r] =
          worldFromGlCamera[r] * px
              + worldFromGlCamera[4 + r] * py
              + worldFromGlCamera[8 + r] * pz
              + worldFromGlCamera[12 + r];
  }

  public static float[] worldFromCvCamera(float[] gl) {
    float[] out = gl.clone();
    for (int r = 0; r < 4; r++) {
      out[4 + r] = -out[4 + r];
      out[8 + r] = -out[8 + r];
    }
    return out;
  }

  public static int rgbFromNv21(byte[] image, int width, int height, int x, int y) {
    x = Math.max(0, Math.min(width - 1, x));
    y = Math.max(0, Math.min(height - 1, y));
    int yy = (image[y * width + x] & 255) - 16;
    int c = width * height + (y / 2) * width + (x / 2) * 2;
    int v = (image[c] & 255) - 128, u = (image[c + 1] & 255) - 128;
    int a = 298 * Math.max(yy, 0);
    int r = clamp((a + 409 * v + 128) >> 8),
        g = clamp((a - 100 * u - 208 * v + 128) >> 8),
        b = clamp((a + 516 * u + 128) >> 8);
    return (r << 16) | (g << 8) | b;
  }

  private static int clamp(int v) {
    return Math.max(0, Math.min(255, v));
  }

  public static int unsignedDepth(byte[] depth, int index) {
    return (depth[index * 2] & 255) | ((depth[index * 2 + 1] & 255) << 8);
  }
}
