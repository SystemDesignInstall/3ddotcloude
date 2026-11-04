package com.systemdesign.capture;

/** Owned camera buffers, handed to the writer without further mutation; no Android objects escape. */
public final class DepthFrame {
  public final long cameraTimestampNs, depthTimestampNs;
  public final int width, height, colorWidth, colorHeight;
  public final byte[] depth, confidence, nv21;
  public final float[] depthIntrinsics, imageIntrinsics, worldFromGlCamera, textureToImageCorners;

  public DepthFrame(
      long ct,
      long dt,
      int w,
      int h,
      int cw,
      int ch,
      byte[] d,
      byte[] conf,
      byte[] color,
      float[] dk,
      float[] ik,
      float[] pose,
      float[] corners) {
    if (w < 1
        || h < 1
        || cw < 2
        || ch < 2
        || cw % 2 != 0
        || ch % 2 != 0
        || d.length != w * h * 2
        || conf.length != w * h
        || color.length != cw * ch * 3 / 2
        || dk.length != 4
        || ik.length != 4
        || pose.length != 16
        || corners.length != 8) throw new IllegalArgumentException("Invalid camera buffers");
    if (dk[0] <= 0 || dk[1] <= 0) throw new IllegalArgumentException("Invalid focal length");
    cameraTimestampNs = ct;
    depthTimestampNs = dt;
    width = w;
    height = h;
    colorWidth = cw;
    colorHeight = ch;
    depth = d;
    confidence = conf;
    nv21 = color;
    depthIntrinsics = dk;
    imageIntrinsics = ik;
    worldFromGlCamera = pose;
    textureToImageCorners = corners;
  }

  public int integrate(Cloud cloud) {
    int step = Math.max(1, (int) Math.ceil(Math.sqrt((double) width * height / 12000)));
    int valid = 0;
    float[] p = new float[3];
    for (int y = 0; y < height; y += step)
      for (int x = 0; x < width; x += step) {
        int i = y * width + x, mm = Geometry.unsignedDepth(depth, i), q = confidence[i] & 255;
        if (mm < 300 || mm > 5000 || q < 128) continue;
        Geometry.unproject(x, y, mm, depthIntrinsics, worldFromGlCamera, p);
        float u = (float) x / width, v = (float) y / height;
        float[] c = textureToImageCorners;
        int ix =
            Math.round(
                (1 - u) * (1 - v) * c[0] + u * (1 - v) * c[2] + (1 - u) * v * c[4] + u * v * c[6]);
        int iy =
            Math.round(
                (1 - u) * (1 - v) * c[1] + u * (1 - v) * c[3] + (1 - u) * v * c[5] + u * v * c[7]);
        if (ix < 0 || iy < 0 || ix >= colorWidth || iy >= colorHeight) continue;
        cloud.add(p[0], p[1], p[2], Geometry.rgbFromNv21(nv21, colorWidth, colorHeight, ix, iy), q);
        valid++;
      }
    return valid;
  }
}
