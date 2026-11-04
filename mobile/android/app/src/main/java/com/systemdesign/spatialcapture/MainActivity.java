package com.systemdesign.spatialcapture;

import android.Manifest;
import android.app.*;
import android.content.*;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.opengl.GLSurfaceView;
import android.os.*;
import android.view.*;
import android.widget.*;
import com.google.ar.core.*;
import com.systemdesign.capture.*;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

public final class MainActivity extends Activity {
  volatile Session session;
  volatile Cloud cloud = new Cloud();
  private volatile boolean recording, finishing, exporting, depthSupported, resumed;
  private volatile CaptureWriter writer;
  private final Object captureLock = new Object();
  private final AtomicBoolean busy = new AtomicBoolean();
  private final AtomicBoolean cameraFailureReported = new AtomicBoolean();
  private final ExecutorService io = Executors.newSingleThreadExecutor();
  private GLSurfaceView surface;
  private TextView status, counts, notice;
  private Button start, stop, ply, zip, scans;
  private File selected, pendingExport;
  private boolean installRequested;
  private long lastWaiting;
  private static final int CAMERA_PERMISSION = 101, EXPORT_PLY = 201, EXPORT_ZIP = 202;

  @Override
  public void onCreate(Bundle state) {
    super.onCreate(state);
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    FrameLayout root = new FrameLayout(this);
    root.setBackgroundColor(Color.rgb(10, 20, 28));
    surface = new GLSurfaceView(this);
    surface.setEGLContextClientVersion(2);
    surface.setEGLConfigChooser(8, 8, 8, 8, 16, 0);
    surface.setPreserveEGLContextOnPause(true);
    surface.setRenderer(new ScanRenderer(this));
    root.addView(surface);
    LinearLayout top = new LinearLayout(this);
    top.setOrientation(LinearLayout.VERTICAL);
    top.setPadding(dp(18), dp(18), dp(18), dp(14));
    top.setBackgroundColor(0xDC101B24);
    top.addView(label("SPATIAL CAPTURE", 23));
    status = label("Проверка камеры…", 15);
    top.addView(status);
    counts = label("0 точек · 0 кадров", 20);
    top.addView(counts);
    top.addView(label("0,3–5 м до поверхности · масштаб требует проверки", 12));
    root.addView(top, new FrameLayout.LayoutParams(-1, -2, Gravity.TOP));
    LinearLayout bottom = new LinearLayout(this);
    bottom.setOrientation(LinearLayout.VERTICAL);
    bottom.setPadding(dp(12), dp(10), dp(12), dp(12));
    bottom.setBackgroundColor(0xEC101B24);
    notice =
        label("Снимайте неподвижную сцену при хорошем свете. Начните с небольшой комнаты.", 14);
    bottom.addView(notice);
    LinearLayout captureRow = new LinearLayout(this);
    start = button("Начать", captureRow);
    stop = button("Стоп / сохранить", captureRow);
    bottom.addView(captureRow);
    LinearLayout exportRow = new LinearLayout(this);
    ply = button("Сохранить PLY", exportRow);
    zip = button("Данные ZIP", exportRow);
    bottom.addView(exportRow);
    scans = new Button(this);
    scans.setText("Мои сканы");
    bottom.addView(scans);
    TextView privacy = label("Google Play Services for AR · сведения и конфиденциальность", 11);
    privacy.setPadding(dp(4), dp(8), 0, 0);
    bottom.addView(privacy);
    privacy.setOnClickListener(
        v ->
            new AlertDialog.Builder(this)
                .setTitle("О приложении")
                .setMessage(
                    "Spatial Capture 0.1.0-preview.1\n\n"
                        + "Съёмка хранится на телефоне. Приложение не загружает её на сервер."
                        + " Экспорт выполняется в выбранное вами место. Удаление приложения удалит"
                        + " неэкспортированные сканы.\n\n"
                        + "This application runs on Google Play Services for AR (ARCore), which is"
                        + " provided by Google LLC and governed by the Google Privacy Policy.\n\n"
                        + "https://policies.google.com/privacy\n"
                        + "https://play.google.com/store/apps/details?id=com.google.ar.core\n\n"
                        + "Расстояния оценены ARCore. Шаг облака 2,5 см не означает точность 2,5"
                        + " см. Глобальная коррекция траектории в этой версии отсутствует.")
                .setPositiveButton("Закрыть", null)
                .setNeutralButton(
                    "Политика Google",
                    (d, w) ->
                        startActivity(
                            new Intent(
                                Intent.ACTION_VIEW,
                                Uri.parse("https://policies.google.com/privacy"))))
                .show());
    root.addView(bottom, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
    root.setOnApplyWindowInsetsListener(
        (v, insets) -> {
          v.setPadding(
              insets.getSystemWindowInsetLeft(),
              insets.getSystemWindowInsetTop(),
              insets.getSystemWindowInsetRight(),
              insets.getSystemWindowInsetBottom());
          return insets;
        });
    setContentView(root);
    root.requestApplyInsets();
    start.setOnClickListener(v -> begin());
    stop.setOnClickListener(v -> stopCapture("user"));
    ply.setOnClickListener(v -> export(EXPORT_PLY));
    zip.setOnClickListener(v -> export(EXPORT_ZIP));
    scans.setOnClickListener(v -> chooseScan());
    File[] saved = scanDirectories();
    if (saved.length > 0) selected = saved[0];
    if (state != null) {
      for (File file : saved) {
        if (file.getName().equals(state.getString("selected_scan"))) selected = file;
        if (file.getName().equals(state.getString("pending_export"))) pendingExport = file;
      }
    }
    updateControls();
  }

  private int dp(int n) {
    return Math.round(n * getResources().getDisplayMetrics().density);
  }

  private TextView label(String text, int size) {
    TextView t = new TextView(this);
    t.setText(text);
    t.setTextSize(size);
    t.setTextColor(Color.WHITE);
    t.setPadding(0, dp(3), 0, dp(3));
    return t;
  }

  private Button button(String text, LinearLayout row) {
    Button b = new Button(this);
    b.setText(text);
    b.setTextSize(12);
    row.addView(b, new LinearLayout.LayoutParams(0, -2, 1));
    return b;
  }

  @Override
  protected void onSaveInstanceState(Bundle state) {
    if (selected != null) state.putString("selected_scan", selected.getName());
    if (pendingExport != null) state.putString("pending_export", pendingExport.getName());
    super.onSaveInstanceState(state);
  }

  @Override
  protected void onResume() {
    super.onResume();
    resumed = true;
    if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
      requestPermissions(new String[] {Manifest.permission.CAMERA}, CAMERA_PERMISSION);
      return;
    }
    resumeCamera();
  }

  private void resumeCamera() {
    if (!resumed) return;
    try {
      if (session == null) {
        if (ArCoreApk.getInstance().requestInstall(this, !installRequested)
            == ArCoreApk.InstallStatus.INSTALL_REQUESTED) {
          installRequested = true;
          status.setText("Установите Google Play Services for AR");
          return;
        }
        Session candidate = new Session(this);
        depthSupported = candidate.isDepthModeSupported(Config.DepthMode.AUTOMATIC);
        if (!depthSupported) {
          candidate.close();
          status.setText("Этот телефон не поддерживает ARCore Depth");
          notice.setText(
              "Сканирование глубины недоступно. Сохранённые сканы можно экспортировать.");
          updateControls();
          return;
        }
        Config config = candidate.getConfig();
        config.setDepthMode(Config.DepthMode.AUTOMATIC);
        config.setFocusMode(Config.FocusMode.AUTO);
        candidate.configure(config);
        session = candidate;
      }
      session.resume();
      depthSupported = true;
      cameraFailureReported.set(false);
      surface.onResume();
      updateControls();
    } catch (Exception e) {
      depthSupported = false;
      status.setText("Не удалось запустить ARCore");
      notice.setText(e.getClass().getSimpleName() + ": " + e.getMessage());
      updateControls();
    }
  }

  @Override
  protected void onPause() {
    resumed = false;
    stopCapture("app_background");
    surface.onPause();
    if (session != null) session.pause();
    super.onPause();
  }

  @Override
  protected void onDestroy() {
    if (session != null) {
      session.close();
      session = null;
    }
    io.shutdown();
    super.onDestroy();
  }

  @Override
  public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
    super.onRequestPermissionsResult(request, permissions, results);
    if (request == CAMERA_PERMISSION) {
      if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) resumeCamera();
      else {
        status.setText("Нужно разрешение на камеру");
        notice.setText("Разрешите камеру в настройках Android и вернитесь в приложение.");
      }
    }
  }

  int rotation() {
    return getWindowManager().getDefaultDisplay().getRotation();
  }

  private File root() {
    return new File(getFilesDir(), "scans");
  }

  private void begin() {
    synchronized (captureLock) {
      if (recording || finishing || exporting || !depthSupported || session == null) return;
      if (getFilesDir().getUsableSpace() < 128L * 1024 * 1024) {
        notice.setText("Освободите минимум 128 МБ на телефоне");
        return;
      }
      try {
        cloud = new Cloud();
        writer = new CaptureWriter(root(), cloud);
        selected = writer.directory;
        recording = true;
        counts.setText("0 точек · 0 кадров");
        notice.setText(
            "Идёт запись. Медленно перемещайте телефон, снимайте поверхности с разных сторон.");
      } catch (Exception e) {
        notice.setText("Ошибка создания скана: " + e.getMessage());
      }
    }
    updateControls();
  }

  boolean canCapture() {
    return recording && !busy.get();
  }

  boolean accept(DepthFrame frame) {
    synchronized (captureLock) {
      if (!recording || !busy.compareAndSet(false, true)) return false;
      CaptureWriter active = writer;
      io.execute(
          () -> {
            try {
              active.append(frame);
              runOnUiThread(
                  () ->
                      counts.setText(
                          String.format(
                              Locale.getDefault(),
                              "%,d точек · %d кадров",
                              active.cloud.size(),
                              active.frameCount())));
              if (active.full() || getFilesDir().getUsableSpace() < 64L * 1024 * 1024)
                runOnUiThread(() -> stopCapture("limit"));
            } catch (Exception e) {
              runOnUiThread(
                  () -> {
                    notice.setText("Ошибка записи: " + e.getMessage());
                    stopCapture("write_error");
                  });
            } finally {
              busy.set(false);
            }
          });
      return true;
    }
  }

  private void stopCapture(String reason) {
    synchronized (captureLock) {
      if (!recording) return;
      recording = false;
      finishing = true;
      CaptureWriter active = writer;
      notice.setText("Сохранение облака…");
      updateControls();
      io.execute(
          () -> {
            try {
              active.finish(reason);
              runOnUiThread(
                  () ->
                      notice.setText(
                          (active.cloud.size() == 0
                                  ? "Точки не получены. Попробуйте больше света и движения. "
                                  : "Скан сохранён. ")
                              + (reason.equals("limit")
                                  ? "Достигнут лимит записи. Начните новый скан."
                                  : reason.equals("write_error")
                                      ? "Запись остановлена из-за ошибки; сохранена принятая часть."
                                      : "Экспортируйте PLY или ZIP с исходными данными.")));
            } catch (Exception e) {
              runOnUiThread(
                  () ->
                      notice.setText(
                          "Не удалось завершить скан: "
                              + e.getMessage()
                              + ". Исходные кадры доступны в ZIP."));
            } finally {
              runOnUiThread(
                  () -> {
                    finishing = false;
                    updateControls();
                  });
            }
          });
    }
  }

  void tracking(String text) {
    runOnUiThread(() -> status.setText(text));
  }

  void depthWaiting() {
    long now = SystemClock.elapsedRealtime();
    if (now - lastWaiting < 3000) return;
    lastWaiting = now;
    runOnUiThread(
        () -> {
          if (recording)
            notice.setText(
                "Ожидание глубины: перемещайте телефон вдоль поверхности, не только вращайте.");
        });
  }

  void cameraError(Exception e) {
    if (!cameraFailureReported.compareAndSet(false, true)) return;
    runOnUiThread(
        () -> {
          depthSupported = false;
          status.setText("Ошибка камеры: " + e.getClass().getSimpleName());
          stopCapture("camera_error");
          updateControls();
        });
  }

  private void updateControls() {
    boolean idle = !recording && !finishing && !exporting;
    start.setEnabled(idle && depthSupported && session != null);
    stop.setEnabled(recording);
    scans.setEnabled(idle);
    zip.setEnabled(idle && selected != null);
    ply.setEnabled(idle && selected != null && new File(selected, "cloud.ply").isFile());
  }

  private File[] scanDirectories() {
    File[] all = root().listFiles(f -> f.isDirectory() && new File(f, "session.json").isFile());
    if (all == null) return new File[0];
    Arrays.sort(all, (a, b) -> b.getName().compareTo(a.getName()));
    return all;
  }

  private void chooseScan() {
    File[] all = scanDirectories();
    if (all.length == 0) {
      notice.setText("Сохранённых сканов пока нет");
      return;
    }
    String[] labels = new String[all.length];
    for (int i = 0; i < all.length; i++)
      labels[i] =
          all[i].getName()
              + (new File(all[i], "manifest.json").isFile() ? "" : " · восстановление из ZIP");
    new AlertDialog.Builder(this)
        .setTitle("Выберите скан для экспорта")
        .setItems(
            labels,
            (dialog, index) -> {
              selected = all[index];
              notice.setText("Выбран: " + labels[index]);
              updateControls();
            })
        .setNegativeButton("Закрыть", null)
        .show();
  }

  private void export(int type) {
    if (selected == null) return;
    pendingExport = selected;
    Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
    intent.addCategory(Intent.CATEGORY_OPENABLE);
    intent.setType(type == EXPORT_PLY ? "application/octet-stream" : "application/zip");
    intent.putExtra(
        Intent.EXTRA_TITLE,
        "SpatialCapture_" + selected.getName() + (type == EXPORT_PLY ? ".ply" : ".zip"));
    try {
      startActivityForResult(intent, type);
    } catch (ActivityNotFoundException e) {
      notice.setText("На телефоне нет системного выбора файлов");
    }
  }

  @Override
  protected void onActivityResult(int request, int result, Intent data) {
    super.onActivityResult(request, result, data);
    if ((request != EXPORT_PLY && request != EXPORT_ZIP)
        || result != RESULT_OK
        || data == null
        || data.getData() == null
        || pendingExport == null) return;
    File source = pendingExport;
    Uri uri = data.getData();
    exporting = true;
    notice.setText("Экспорт…");
    updateControls();
    io.execute(
        () -> {
          try (OutputStream out = getContentResolver().openOutputStream(uri, "wt")) {
            if (out == null) throw new IOException("Cannot open destination");
            if (request == EXPORT_ZIP) CaptureWriter.zip(source, out);
            else
              try (InputStream in = new FileInputStream(new File(source, "cloud.ply"))) {
                byte[] b = new byte[65536];
                int n;
                while ((n = in.read(b)) != -1) out.write(b, 0, n);
              }
            runOnUiThread(() -> notice.setText("Файл экспортирован в выбранное место."));
          } catch (Exception e) {
            runOnUiThread(
                () ->
                    notice.setText(
                        "Экспорт не завершён: "
                            + e.getMessage()
                            + ". Исходный скан сохранён в приложении."));
          } finally {
            runOnUiThread(
                () -> {
                  exporting = false;
                  updateControls();
                });
          }
        });
  }
}
