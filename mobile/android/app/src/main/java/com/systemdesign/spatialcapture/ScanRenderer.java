package com.systemdesign.spatialcapture;

import android.media.Image;
import android.opengl.*;
import com.google.ar.core.*;
import com.google.ar.core.exceptions.NotYetAvailableException;
import com.systemdesign.capture.*;
import java.nio.*;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

final class ScanRenderer implements GLSurfaceView.Renderer {
  private final MainActivity owner;
  private int cameraTexture, backgroundProgram, pointProgram, width = 1, height = 1;
  private final FloatBuffer quad = floats(new float[] {-1, -1, 1, -1, -1, 1, 1, 1});
  private final FloatBuffer uv = floats(new float[8]);
  private FloatBuffer points;
  private Cloud renderedCloud;
  private long lastSnapshot, lastSample, lastDepth = -1, lastStatus;
  private final float[] projection = new float[16], view = new float[16], vp = new float[16];

  ScanRenderer(MainActivity owner) {
    this.owner = owner;
  }

  @Override
  public void onSurfaceCreated(GL10 gl, EGLConfig config) {
    int[] texture = new int[1];
    GLES20.glGenTextures(1, texture, 0);
    cameraTexture = texture[0];
    GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, cameraTexture);
    GLES20.glTexParameteri(
        GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
    GLES20.glTexParameteri(
        GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
    GLES20.glTexParameteri(
        GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
    GLES20.glTexParameteri(
        GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
    backgroundProgram =
        program(
            "attribute vec2 a; attribute vec2 t; varying vec2 uv; void"
                + " main(){gl_Position=vec4(a,0.,1.);uv=t;}",
            "#extension GL_OES_EGL_image_external : require\n"
                + "precision mediump float; uniform samplerExternalOES camera; varying vec2 uv;"
                + " void main(){gl_FragColor=texture2D(camera,uv);}");
    pointProgram =
        program(
            "uniform mat4 vp; attribute vec3 p; attribute vec3 c; varying vec3 rgb; void"
                + " main(){gl_Position=vp*vec4(p,1.);gl_PointSize=3.;rgb=c;}",
            "precision mediump float; varying vec3 rgb; void"
                + " main(){if(distance(gl_PointCoord,vec2(.5))>.5)discard;gl_FragColor=vec4(rgb,1.);}");
  }

  @Override
  public void onSurfaceChanged(GL10 gl, int w, int h) {
    width = w;
    height = h;
    GLES20.glViewport(0, 0, w, h);
  }

  @Override
  public void onDrawFrame(GL10 gl) {
    GLES20.glClearColor(.04f, .08f, .11f, 1);
    GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT | GLES20.GL_DEPTH_BUFFER_BIT);
    Session session = owner.session;
    if (session == null) return;
    try {
      session.setCameraTextureName(cameraTexture);
      session.setDisplayGeometry(owner.rotation(), width, height);
      Frame frame = session.update();
      if (frame.getTimestamp() == 0) return;
      frame.transformCoordinates2d(
          Coordinates2d.OPENGL_NORMALIZED_DEVICE_COORDINATES,
          quad,
          Coordinates2d.TEXTURE_NORMALIZED,
          uv);
      drawCamera();
      Camera camera = frame.getCamera();
      long now = android.os.SystemClock.elapsedRealtime();
      if (camera.getTrackingState() != TrackingState.TRACKING) {
        if (now - lastStatus > 600) {
          owner.tracking("Отслеживание потеряно: " + camera.getTrackingFailureReason());
          lastStatus = now;
        }
        return;
      }
      if (now - lastStatus > 600) {
        owner.tracking("Камера отслеживается · двигайтесь медленно");
        lastStatus = now;
      }
      Cloud cloud = owner.cloud;
      if (cloud != renderedCloud || now - lastSnapshot > 300) {
        points = cloud.vertices();
        renderedCloud = cloud;
        lastSnapshot = now;
      }
      camera.getProjectionMatrix(projection, 0, .1f, 100);
      camera.getViewMatrix(view, 0);
      Matrix.multiplyMM(vp, 0, projection, 0, view, 0);
      drawPoints();
      if (owner.canCapture() && now - lastSample >= 333) {
        try (Image depth = frame.acquireRawDepthImage16Bits();
            Image confidence = frame.acquireRawDepthConfidenceImage();
            Image rgb = frame.acquireCameraImage()) {
          if (depth.getTimestamp() != lastDepth) {
            DepthFrame data = CameraCopy.copy(frame, depth, confidence, rgb);
            if (owner.accept(data)) {
              lastDepth = depth.getTimestamp();
              lastSample = now;
            }
          }
        } catch (NotYetAvailableException ignored) {
          if (now - lastSample > 1500) owner.depthWaiting();
        }
      }
    } catch (Exception error) {
      owner.cameraError(error);
    }
  }

  private void drawCamera() {
    GLES20.glDisable(GLES20.GL_DEPTH_TEST);
    GLES20.glDepthMask(false);
    GLES20.glUseProgram(backgroundProgram);
    int a = GLES20.glGetAttribLocation(backgroundProgram, "a"),
        t = GLES20.glGetAttribLocation(backgroundProgram, "t");
    quad.rewind();
    uv.rewind();
    GLES20.glVertexAttribPointer(a, 2, GLES20.GL_FLOAT, false, 0, quad);
    GLES20.glVertexAttribPointer(t, 2, GLES20.GL_FLOAT, false, 0, uv);
    GLES20.glEnableVertexAttribArray(a);
    GLES20.glEnableVertexAttribArray(t);
    GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
    GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, cameraTexture);
    GLES20.glUniform1i(GLES20.glGetUniformLocation(backgroundProgram, "camera"), 0);
    GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
    GLES20.glDisableVertexAttribArray(a);
    GLES20.glDisableVertexAttribArray(t);
    GLES20.glDepthMask(true);
  }

  private void drawPoints() {
    if (points == null || points.limit() == 0) return;
    GLES20.glEnable(GLES20.GL_DEPTH_TEST);
    GLES20.glUseProgram(pointProgram);
    GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(pointProgram, "vp"), 1, false, vp, 0);
    int p = GLES20.glGetAttribLocation(pointProgram, "p"),
        c = GLES20.glGetAttribLocation(pointProgram, "c");
    points.position(0);
    GLES20.glVertexAttribPointer(p, 3, GLES20.GL_FLOAT, false, 24, points);
    points.position(3);
    GLES20.glVertexAttribPointer(c, 3, GLES20.GL_FLOAT, false, 24, points);
    GLES20.glEnableVertexAttribArray(p);
    GLES20.glEnableVertexAttribArray(c);
    GLES20.glDrawArrays(GLES20.GL_POINTS, 0, points.limit() / 6);
    GLES20.glDisableVertexAttribArray(p);
    GLES20.glDisableVertexAttribArray(c);
    points.rewind();
  }

  private static FloatBuffer floats(float[] data) {
    FloatBuffer f =
        ByteBuffer.allocateDirect(data.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
    f.put(data).rewind();
    return f;
  }

  private static int shader(int type, String source) {
    int s = GLES20.glCreateShader(type);
    GLES20.glShaderSource(s, source);
    GLES20.glCompileShader(s);
    int[] ok = new int[1];
    GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
    if (ok[0] == 0) throw new IllegalStateException(GLES20.glGetShaderInfoLog(s));
    return s;
  }

  private static int program(String vs, String fs) {
    int v = shader(GLES20.GL_VERTEX_SHADER, vs),
        f = shader(GLES20.GL_FRAGMENT_SHADER, fs),
        p = GLES20.glCreateProgram();
    GLES20.glAttachShader(p, v);
    GLES20.glAttachShader(p, f);
    GLES20.glLinkProgram(p);
    int[] ok = new int[1];
    GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0);
    GLES20.glDeleteShader(v);
    GLES20.glDeleteShader(f);
    if (ok[0] == 0) throw new IllegalStateException(GLES20.glGetProgramInfoLog(p));
    return p;
  }
}
