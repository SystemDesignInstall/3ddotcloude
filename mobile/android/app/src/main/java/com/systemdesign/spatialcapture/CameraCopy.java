package com.systemdesign.spatialcapture;

import android.media.Image;
import com.google.ar.core.*;
import com.systemdesign.capture.DepthFrame;
import java.nio.*;

final class CameraCopy {
  private CameraCopy() {}

  static DepthFrame copy(Frame frame, Image depth, Image confidence, Image camera) {
    CameraIntrinsics texture = frame.getCamera().getTextureIntrinsics(),
        image = frame.getCamera().getImageIntrinsics();
    int w = depth.getWidth(), h = depth.getHeight();
    int[] dim = texture.getImageDimensions();
    float[] f = texture.getFocalLength(), p = texture.getPrincipalPoint();
    float[] dk = {f[0] * w / dim[0], f[1] * h / dim[1], p[0] * w / dim[0], p[1] * h / dim[1]};
    f = image.getFocalLength();
    p = image.getPrincipalPoint();
    float[] ik = {f[0], f[1], p[0], p[1]};
    float[] pose = new float[16];
    frame.getCamera().getPose().toMatrix(pose, 0);
    FloatBuffer input =
        ByteBuffer.allocateDirect(32).order(ByteOrder.nativeOrder()).asFloatBuffer();
    FloatBuffer output =
        ByteBuffer.allocateDirect(32).order(ByteOrder.nativeOrder()).asFloatBuffer();
    input.put(new float[] {0, 0, 1, 0, 0, 1, 1, 1}).rewind();
    frame.transformCoordinates2d(
        Coordinates2d.TEXTURE_NORMALIZED, input, Coordinates2d.IMAGE_PIXELS, output);
    float[] corners = new float[8];
    output.rewind();
    output.get(corners);
    int cw = camera.getWidth(), ch = camera.getHeight();
    if (cw % 2 != 0 || ch % 2 != 0) throw new IllegalArgumentException("Odd YUV dimensions");
    byte[] nv21 = new byte[cw * ch * 3 / 2];
    copyPlane(camera.getPlanes()[0], cw, ch, 1, nv21, 0, 1);
    copyPlane(camera.getPlanes()[2], cw / 2, ch / 2, 1, nv21, cw * ch, 2);
    copyPlane(camera.getPlanes()[1], cw / 2, ch / 2, 1, nv21, cw * ch + 1, 2);
    byte[] depths = new byte[w * h * 2], qualities = new byte[w * h];
    copyPlane(depth.getPlanes()[0], w, h, 2, depths, 0, 2);
    copyPlane(confidence.getPlanes()[0], w, h, 1, qualities, 0, 1);
    return new DepthFrame(
        camera.getTimestamp(),
        depth.getTimestamp(),
        w,
        h,
        cw,
        ch,
        depths,
        qualities,
        nv21,
        dk,
        ik,
        pose,
        corners);
  }

  private static void copyPlane(
      Image.Plane plane, int w, int h, int bytes, byte[] out, int offset, int outStride) {
    ByteBuffer b = plane.getBuffer().duplicate();
    int base = b.position();
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        for (int k = 0; k < bytes; k++)
          out[offset + (y * w + x) * outStride + k] =
              b.get(base + y * plane.getRowStride() + x * plane.getPixelStride() + k);
  }
}
