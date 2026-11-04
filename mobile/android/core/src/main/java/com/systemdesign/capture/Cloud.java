package com.systemdesign.capture;

import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;

/** Bounded voxel cache. Highest-confidence observation wins; no interpolated geometry. */
public final class Cloud {
  public static final int MAX_POINTS = 300000;
  public static final float VOXEL_METERS = .025f;
  private final int capacity;
  private final float[] xyz;
  private final int[] rgb;
  private final byte[] confidence;
  private final HashMap<Long, Integer> voxels = new HashMap<>();
  private int count;

  public Cloud() {
    this(MAX_POINTS);
  }

  public Cloud(int capacity) {
    this.capacity = capacity;
    xyz = new float[capacity * 3];
    rgb = new int[capacity];
    confidence = new byte[capacity];
  }

  public synchronized boolean add(float x, float y, float z, int color, int quality) {
    if (!Float.isFinite(x)
        || !Float.isFinite(y)
        || !Float.isFinite(z)
        || quality < 128
        || quality > 255) return false;
    int a = (int) Math.floor(x / VOXEL_METERS),
        b = (int) Math.floor(y / VOXEL_METERS),
        c = (int) Math.floor(z / VOXEL_METERS);
    if (Math.abs((long) a) > 1048575
        || Math.abs((long) b) > 1048575
        || Math.abs((long) c) > 1048575) return false;
    long key = ((a & 0x1fffffL) << 42) | ((b & 0x1fffffL) << 21) | (c & 0x1fffffL);
    Integer existing = voxels.get(key);
    int i;
    if (existing != null) {
      i = existing;
      if ((confidence[i] & 255) >= quality) return false;
    } else {
      if (count == capacity) return false;
      i = count++;
      voxels.put(key, i);
    }
    xyz[i * 3] = x;
    xyz[i * 3 + 1] = y;
    xyz[i * 3 + 2] = z;
    rgb[i] = color;
    confidence[i] = (byte) quality;
    return true;
  }

  public synchronized int size() {
    return count;
  }

  public synchronized FloatBuffer vertices() {
    FloatBuffer out =
        ByteBuffer.allocateDirect(count * 6 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
    for (int i = 0; i < count; i++)
      out.put(xyz, i * 3, 3)
          .put(((rgb[i] >> 16) & 255) / 255f)
          .put(((rgb[i] >> 8) & 255) / 255f)
          .put((rgb[i] & 255) / 255f);
    out.flip();
    return out;
  }

  public synchronized void writePly(OutputStream stream) throws IOException {
    String header =
        "ply\n"
            + "format binary_little_endian 1.0\n"
            + "comment units meters\n"
            + "comment frame arcore_session_world right_handed y_up\n"
            + "comment confidence is ARCore estimate, not measurement accuracy\n"
            + "element vertex "
            + count
            + "\n"
            + "property float x\n"
            + "property float y\n"
            + "property float z\n"
            + "property uchar red\n"
            + "property uchar green\n"
            + "property uchar blue\n"
            + "property uchar confidence\n"
            + "end_header\n";
    stream.write(header.getBytes(StandardCharsets.US_ASCII));
    ByteBuffer row = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
    for (int i = 0; i < count; i++) {
      row.clear();
      row.putFloat(xyz[i * 3])
          .putFloat(xyz[i * 3 + 1])
          .putFloat(xyz[i * 3 + 2])
          .put((byte) (rgb[i] >> 16))
          .put((byte) (rgb[i] >> 8))
          .put((byte) rgb[i])
          .put(confidence[i]);
      stream.write(row.array());
    }
  }
}
