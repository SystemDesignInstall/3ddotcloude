package com.systemdesign.capture;

import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;

public final class CoreChecks {
  private static int checks;

  private static void check(boolean ok, String message) {
    checks++;
    if (!ok) throw new AssertionError(message);
  }

  private static boolean near(float a, float b) {
    return Math.abs(a - b) < 1e-5;
  }

  public static void main(String[] args) throws Exception {
    float[] identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float[] p = new float[3];
    Geometry.unproject(2, 2, 2000, new float[] {2, 2, 2, 2}, identity, p);
    check(near(p[0], 0) && near(p[1], 0) && near(p[2], -2), "center is in front of AR camera");
    Geometry.unproject(3, 3, 2000, new float[] {2, 2, 2, 2}, identity, p);
    check(near(p[0], 1) && near(p[1], -1) && near(p[2], -2), "pixel axes are not mirrored");
    float[] cv = Geometry.worldFromCvCamera(identity);
    check(cv[0] == 1 && cv[5] == -1 && cv[10] == -1 && cv[15] == 1, "OpenCV camera conversion");
    float[] translated = identity.clone();
    translated[12] = 10;
    translated[13] = 2;
    translated[14] = 3;
    Geometry.unproject(2, 2, 1000, new float[] {2, 2, 2, 2}, translated, p);
    check(
        near(p[0], 10) && near(p[1], 2) && near(p[2], 2), "world_from_camera transform direction");
    check(
        Geometry.unsignedDepth(new byte[] {(byte) 0xff, (byte) 0xff}, 0) == 65535,
        "unsigned depth");
    byte[] nv21 = {16, 16, 16, 16, (byte) 128, (byte) 128};
    check(Geometry.rgbFromNv21(nv21, 2, 2, 0, 0) == 0, "black NV21");
    nv21[0] = (byte) 235;
    check(Geometry.rgbFromNv21(nv21, 2, 2, 0, 0) == 0xffffff, "white NV21");
    Cloud cloud = new Cloud(2);
    check(cloud.add(-.001f, 0, -1, 0xff0000, 130), "negative voxel");
    check(cloud.add(.001f, 0, -1, 0x00ff00, 200), "positive adjacent voxel");
    check(!cloud.add(2, 0, -1, 0, 255) && cloud.size() == 2, "bounded memory");
    check(!cloud.add(Float.NaN, 0, 0, 0, 255), "invalid geometry rejected");
    check(!cloud.add(.001f, 0, -1, 0, 127), "low confidence rejected");
    check(cloud.add(.002f, 0, -1, 0x0000ff, 240) && cloud.size() == 2, "confidence replacement");
    ByteArrayOutputStream stream = new ByteArrayOutputStream();
    cloud.writePly(stream);
    byte[] output = stream.toByteArray();
    String ascii = new String(output, StandardCharsets.ISO_8859_1);
    int header = ascii.indexOf("end_header\n") + 11;
    check(
        ascii.contains("element vertex 2\n") && output.length == header + 32,
        "PLY record count and size");
    ByteBuffer b = ByteBuffer.wrap(output, header, 32).order(ByteOrder.LITTLE_ENDIAN);
    check(near(b.getFloat(), -.001f), "PLY float little endian");
    b.position(header + 16 + 12);
    check(
        (b.get() & 255) == 0
            && (b.get() & 255) == 0
            && (b.get() & 255) == 255
            && (b.get() & 255) == 240,
        "PLY RGB confidence");
    byte[] d = {(byte) 232, 3, 0, 0, (byte) 232, 3, (byte) 232, 3};
    byte[] q = {(byte) 255, (byte) 255, 0, (byte) 255};
    DepthFrame f =
        new DepthFrame(
            1,
            1,
            2,
            2,
            2,
            2,
            d,
            q,
            nv21,
            new float[] {2, 2, 0, 0},
            new float[] {2, 2, 0, 0},
            identity,
            new float[] {0, 0, 2, 0, 0, 2, 2, 2});
    Cloud sampled = new Cloud(10);
    check(
        f.integrate(sampled) == 2 && sampled.size() == 2, "missing depth and confidence filtering");
    if (args.length > 0) Files.write(Path.of(args[0]), output);
    System.out.println("PASS " + checks + " core checks");
  }
}
