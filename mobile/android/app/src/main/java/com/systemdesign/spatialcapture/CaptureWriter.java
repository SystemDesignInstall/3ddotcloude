package com.systemdesign.spatialcapture;

import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.YuvImage;
import android.os.Build;
import com.systemdesign.capture.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.text.SimpleDateFormat;
import java.util.*;
import java.util.zip.*;
import org.json.*;

/** All methods run on one IO executor. Each observation commits by directory rename. */
final class CaptureWriter {
  static final long MAX_BYTES = 512L * 1024 * 1024;
  static final int MAX_FRAMES = 900;
  final File directory;
  final Cloud cloud;
  private final JSONObject session;
  private int frames;
  private long bytes;
  private long firstTimestamp = -1;

  CaptureWriter(File root, Cloud cloud) throws Exception {
    this.cloud = cloud;
    String id = UUID.randomUUID().toString();
    String time = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
    directory = new File(root, time + "_" + id.substring(0, 8));
    if (!new File(directory, "frames").mkdirs())
      throw new IOException("Cannot create scan directory");
    session =
        new JSONObject()
            .put("format", "spatial-capture")
            .put("format_version", 1)
            .put("session_id", id)
            .put("producer", "Spatial Capture 0.1.0-preview.1")
            .put(
                "created_utc",
                new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ssXXX", Locale.US) {
                  {
                    setTimeZone(TimeZone.getTimeZone("UTC"));
                  }
                }.format(new Date()))
            .put("device", Build.MANUFACTURER + " " + Build.MODEL)
            .put("android_api", Build.VERSION.SDK_INT)
            .put("world_frame_id", id + "/arcore_world")
            .put("camera_frame_id", id + "/camera_cv")
            .put("world_axes", "right-handed, +Y up, ARCore session-local; not georeferenced")
            .put("camera_axes", "OpenCV: +X right, +Y down, +Z forward")
            .put("pose_direction", "world_from_camera_cv")
            .put("matrix_layout", "column-major")
            .put("distance_unit", "meter")
            .put("depth_unit", "millimeter")
            .put("time_domain", "ARCore device monotonic nanoseconds; not Unix time")
            .put("metric_basis", "ARCore estimated metric scale; not externally validated")
            .put("depth_source", "ARCore Raw Depth estimates, not guaranteed hardware depth")
            .put("voxel_m", Cloud.VOXEL_METERS)
            .put("min_depth_m", .3)
            .put("max_depth_m", 5)
            .put("min_confidence_u8", 128)
            .put("point_limit", Cloud.MAX_POINTS)
            .put("global_loop_correction", false)
            .put("state", "recording");
    writeJson(new File(directory, "session.json"), session);
  }

  boolean full() {
    return frames >= MAX_FRAMES || bytes >= MAX_BYTES || cloud.size() >= Cloud.MAX_POINTS;
  }

  int frameCount() {
    return frames;
  }

  void append(DepthFrame frame) throws Exception {
    File pending = new File(directory, String.format(Locale.US, ".pending-%06d", frames));
    if (!pending.mkdir()) throw new IOException("Cannot create observation");
    writeBytes(new File(pending, "depth.u16le"), frame.depth);
    writeBytes(new File(pending, "confidence.u8"), frame.confidence);
    try (OutputStream s =
        new GZIPOutputStream(
            new BufferedOutputStream(new FileOutputStream(new File(pending, "color.nv21.gz"))))) {
      s.write(frame.nv21);
    }
    try (OutputStream s =
        new BufferedOutputStream(new FileOutputStream(new File(pending, "color.jpg")))) {
      if (!new YuvImage(frame.nv21, ImageFormat.NV21, frame.colorWidth, frame.colorHeight, null)
          .compressToJpeg(new Rect(0, 0, frame.colorWidth, frame.colorHeight), 95, s))
        throw new IOException("JPEG encoding failed");
    }
    JSONObject hashes = new JSONObject();
    long frameBytes = 0;
    for (String name :
        new String[] {"depth.u16le", "confidence.u8", "color.nv21.gz", "color.jpg"}) {
      File f = new File(pending, name);
      hashes.put(name, sha256(f));
      frameBytes += f.length();
    }
    JSONObject meta =
        new JSONObject()
            .put("index", frames)
            .put("camera_timestamp_ns", Long.toString(frame.cameraTimestampNs))
            .put("depth_timestamp_ns", Long.toString(frame.depthTimestampNs))
            .put("depth_width", frame.width)
            .put("depth_height", frame.height)
            .put("color_width", frame.colorWidth)
            .put("color_height", frame.colorHeight)
            .put("depth_intrinsics_fx_fy_cx_cy", array(frame.depthIntrinsics))
            .put("image_intrinsics_fx_fy_cx_cy", array(frame.imageIntrinsics))
            .put("world_from_camera_cv", array(Geometry.worldFromCvCamera(frame.worldFromGlCamera)))
            .put("texture_to_image_corners", array(frame.textureToImageCorners))
            .put("corner_order", "(0,0),(1,0),(0,1),(1,1)")
            .put("tracking_state", "TRACKING")
            .put("sha256", hashes);
    writeJson(new File(pending, "frame.json"), meta);
    if (!pending.renameTo(new File(directory, String.format(Locale.US, "frames/%06d", frames))))
      throw new IOException("Observation commit failed");
    frame.integrate(cloud);
    if (firstTimestamp < 0) firstTimestamp = frame.cameraTimestampNs;
    frames++;
    bytes += frameBytes;
  }

  void finish(String reason) throws Exception {
    File temp = new File(directory, "cloud.ply.partial");
    try (OutputStream s = new BufferedOutputStream(new FileOutputStream(temp))) {
      cloud.writePly(s);
    }
    File target = new File(directory, "cloud.ply");
    if (!temp.renameTo(target)) throw new IOException("Cloud commit failed");
    JSONObject finalState =
        new JSONObject(session.toString())
            .put("state", "complete")
            .put("stop_reason", reason)
            .put("frame_count", frames)
            .put("point_count", cloud.size())
            .put("first_camera_timestamp_ns", Long.toString(firstTimestamp))
            .put("cloud_sha256", sha256(target))
            .put("payload_bytes", bytes);
    File manifest = new File(directory, "manifest.json.partial");
    writeJson(manifest, finalState);
    if (!manifest.renameTo(new File(directory, "manifest.json")))
      throw new IOException("Manifest commit failed");
  }

  static void zip(File directory, OutputStream destination) throws IOException {
    try (ZipOutputStream zip = new ZipOutputStream(new BufferedOutputStream(destination))) {
      zipTree(directory, directory, zip);
    }
  }

  private static void zipTree(File root, File folder, ZipOutputStream zip) throws IOException {
    File[] entries = folder.listFiles();
    if (entries == null) throw new IOException("Cannot read scan");
    Arrays.sort(entries, Comparator.comparing(File::getName));
    byte[] buf = new byte[65536];
    for (File file : entries) {
      if (file.getName().startsWith(".") || file.getName().endsWith(".partial")) continue;
      if (file.isDirectory()) {
        zipTree(root, file, zip);
        continue;
      }
      zip.putNextEntry(new ZipEntry(root.toURI().relativize(file.toURI()).getPath()));
      try (InputStream in = new FileInputStream(file)) {
        int n;
        while ((n = in.read(buf)) != -1) zip.write(buf, 0, n);
      }
      zip.closeEntry();
    }
  }

  private static JSONArray array(float[] a) throws JSONException {
    JSONArray j = new JSONArray();
    for (float f : a) j.put((double) f);
    return j;
  }

  private static void writeBytes(File f, byte[] data) throws IOException {
    try (FileOutputStream out = new FileOutputStream(f)) {
      out.write(data);
      out.getFD().sync();
    }
  }

  private static void writeJson(File f, JSONObject data) throws Exception {
    writeBytes(f, (data.toString(2) + "\n").getBytes(StandardCharsets.UTF_8));
  }

  private static String sha256(File f) throws Exception {
    MessageDigest md = MessageDigest.getInstance("SHA-256");
    byte[] b = new byte[65536];
    try (InputStream in = new FileInputStream(f)) {
      int n;
      while ((n = in.read(b)) != -1) md.update(b, 0, n);
    }
    StringBuilder s = new StringBuilder();
    for (byte v : md.digest()) s.append(String.format(Locale.US, "%02x", v & 255));
    return s.toString();
  }
}
