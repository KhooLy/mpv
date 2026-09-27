package org.mpv.androidtest;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.net.Uri;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Locale;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final int PICK_FILE = 42;

    static {
        System.loadLibrary("mpv");
        System.loadLibrary("mpvtest");
    }

    private SurfaceView surfaceView;
    private TextView status;
    private boolean surfaceReady;
    private long mpv;
    private String pendingFile;
    private boolean pendingToneMapToSdr;
    private boolean activeToneMapToSdr;
    private boolean activePreferBaseLayer;
    private String extraOpts;

    private static native long nativeCreate(Surface surface, String path,
                                            int toneMapToSdr, int preferBaseLayer,
                                            String extraOpts);
    private static native int nativeLoad(long handle, String path);
    private static native void nativeDestroy(long handle);
    private static native int nativeCommand(long handle, String cmd);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        buildUi();
        handleIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        handleIntent(intent);
    }

    private void handleIntent(Intent intent) {
        if (intent == null)
            return;
        String cmd = intent.getStringExtra("cmd");
        if (cmd != null) {
            if (mpv != 0)
                nativeCommand(mpv, cmd);
            return;
        }
        String opts = intent.getStringExtra("opts");
        if (opts != null && !opts.equals(extraOpts) && mpv != 0) {
            nativeDestroy(mpv);
            mpv = 0;
        }
        if (opts != null)
            extraOpts = opts;
        boolean toneMap = intent.getBooleanExtra("tonemap", false);
        String asset = intent.getStringExtra("asset");
        String path = intent.getStringExtra("path");
        if (asset != null)
            playAsset(asset, toneMap);
        else if (path != null)
            startOrLoad(path, toneMap);
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.BLACK);

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        root.addView(surfaceView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        ScrollView panel = new ScrollView(this);
        LinearLayout controls = new LinearLayout(this);
        controls.setOrientation(LinearLayout.VERTICAL);
        controls.setPadding(18, 10, 18, 12);

        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(12);
        status.setText(deviceReport());
        controls.addView(status);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        addAssetButton(buttons, "SDR", "sdr.mp4");
        addAssetButton(buttons, "HDR10", "hdr10.mp4");
        addAssetButton(buttons, "HLG", "hlg.mp4");
        controls.addView(buttons);

        Button toneMap = new Button(this);
        toneMap.setText("HDR10 → SDR (MediaCodec)");
        toneMap.setOnClickListener(v -> playAssetWithToneMap("hdr10.mp4"));
        controls.addView(toneMap);

        Button hlgToneMap = new Button(this);
        hlgToneMap.setText("HLG → SDR (MediaCodec)");
        hlgToneMap.setOnClickListener(v -> playAssetWithToneMap("hlg.mp4"));
        controls.addView(hlgToneMap);

        Button open = new Button(this);
        open.setText("Open video");
        open.setOnClickListener(v -> openFile());
        controls.addView(open);

        panel.addView(controls);
        root.addView(panel, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(root);
    }

    private void addAssetButton(LinearLayout parent, String label, String asset) {
        Button button = new Button(this);
        button.setText(label);
        button.setOnClickListener(v -> playAsset(asset));
        parent.addView(button, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1));
    }

    private String deviceReport() {
        boolean dv = false;
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
            if (info.isEncoder())
                continue;
            for (String type : info.getSupportedTypes()) {
                if ("video/dolby-vision".equalsIgnoreCase(type))
                    dv = true;
            }
        }

        String hdr = "unknown";
        if (getDisplay() != null && getDisplay().getHdrCapabilities() != null) {
            int[] types = getDisplay().getHdrCapabilities().getSupportedHdrTypes();
            StringBuilder out = new StringBuilder();
            for (int type : types) {
                if (out.length() > 0)
                    out.append(", ");
                out.append(type == 2 ? "HDR10" : type == 3 ? "HLG" :
                        type == 4 ? "HDR10+" : String.valueOf(type));
            }
            hdr = out.toString();
        }

        return String.format(Locale.US,
                "Display HDR: %s | MediaCodec DV decoder: %s\n"
                        + "VO: mediacodec_embed | HWDEC: mediacodec | SurfaceView",
                hdr, dv ? "YES" : "NO (DV fallback only)");
    }

    private boolean displaySupportsHdr() {
        return displaySupportsHdr10() || displaySupportsHlg();
    }

    private boolean displaySupportsHdr10() {
        return displaySupportsHdrType(2); // Display.HdrCapabilities.HDR_TYPE_HDR10
    }

    private boolean displaySupportsHlg() {
        return displaySupportsHdrType(3); // Display.HdrCapabilities.HDR_TYPE_HLG
    }

    private boolean displaySupportsHdrType(int wantedType) {
        if (getDisplay() == null || getDisplay().getHdrCapabilities() == null)
            return false;
        for (int type : getDisplay().getHdrCapabilities().getSupportedHdrTypes()) {
            if (type == wantedType)
                return true;
        }
        return false;
    }

    private boolean displaySupportsDolbyVision() {
        if (getDisplay() == null || getDisplay().getHdrCapabilities() == null)
            return false;
        for (int type : getDisplay().getHdrCapabilities().getSupportedHdrTypes()) {
            if (type == 1) // Display.HdrCapabilities.HDR_TYPE_DOLBY_VISION
                return true;
        }
        return false;
    }

    private void playAsset(String name) {
        playAsset(name, false);
    }

    private void playAssetWithToneMap(String name) {
        playAsset(name, true);
    }

    private void playAsset(String name, boolean toneMapToSdr) {
        try {
            File out = new File(getCacheDir(), name);
            copyAsset(name, out);
            startOrLoad(out.getAbsolutePath(), toneMapToSdr);
        } catch (Exception e) {
            setStatus("Asset error: " + e.getMessage());
        }
    }

    private void copyAsset(String name, File out) throws Exception {
        try (InputStream in = getAssets().open(name);
             FileOutputStream stream = new FileOutputStream(out)) {
            byte[] buffer = new byte[64 * 1024];
            int n;
            while ((n = in.read(buffer)) >= 0)
                stream.write(buffer, 0, n);
        }
    }

    private void openFile() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.setType("video/*");
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        startActivityForResult(intent, PICK_FILE);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_FILE || result != RESULT_OK || data == null || data.getData() == null)
            return;
        try {
            Uri uri = data.getData();
            File out = new File(getCacheDir(), "selected-video" + extension(uri));
            try (InputStream in = getContentResolver().openInputStream(uri);
                 FileOutputStream stream = new FileOutputStream(out)) {
                byte[] buffer = new byte[64 * 1024];
                int n;
                while ((n = in.read(buffer)) >= 0)
                    stream.write(buffer, 0, n);
            }
            startOrLoad(out.getAbsolutePath(), false);
        } catch (Exception e) {
            setStatus("Open error: " + e.getMessage());
        }
    }

    private String extension(Uri uri) {
        String value = uri.getLastPathSegment();
        int dot = value == null ? -1 : value.lastIndexOf('.');
        return dot >= 0 ? value.substring(dot) : ".mp4";
    }

    private void startOrLoad(String path) {
        startOrLoad(path, false);
    }

    private void startOrLoad(String path, boolean toneMapToSdr) {
        VideoKind video = probeVideo(path);
        boolean preferBaseLayer = false;
        pendingFile = path;
        pendingToneMapToSdr = toneMapToSdr;
        if (!surfaceReady)
            return;
        if (mpv != 0 && (activeToneMapToSdr != toneMapToSdr ||
                         activePreferBaseLayer != preferBaseLayer)) {
            nativeDestroy(mpv);
            mpv = 0;
        }
        if (mpv == 0) {
            mpv = nativeCreate(surfaceView.getHolder().getSurface(), path,
                    toneMapToSdr ? 1 : 0,
                    preferBaseLayer ? 1 : 0, extraOpts);
            activeToneMapToSdr = toneMapToSdr;
            activePreferBaseLayer = preferBaseLayer;
            setStatus(mpv == 0 ? "MPV initialization failed" :
                    "Playing: " + path + " [" + video.description() + "]" +
                    (toneMapToSdr ? " (MediaCodec tone-map request)" :
                     preferBaseLayer ? " (DV base-layer fallback)" : " (native output)"));
        } else {
            int rc = nativeLoad(mpv, path);
            setStatus(rc < 0 ? "MPV load failed: " + rc :
                    "Playing: " + path + " [" + video.description() + "]");
        }
    }

    private VideoKind probeVideo(String path) {
        if (path.contains("://"))
            return new VideoKind(false, false, false, false, false, "unknown");
        MediaExtractor extractor = new MediaExtractor();
        try {
            extractor.setDataSource(path);
            for (int i = 0; i < extractor.getTrackCount(); i++) {
                MediaFormat format = extractor.getTrackFormat(i);
                String mime = format.getString(MediaFormat.KEY_MIME);
                if (mime == null || !mime.startsWith("video/"))
                    continue;

                boolean dv = "video/dolby-vision".equalsIgnoreCase(mime);
                int transfer = format.containsKey(MediaFormat.KEY_COLOR_TRANSFER) ?
                        format.getInteger(MediaFormat.KEY_COLOR_TRANSFER) : -1;
                boolean hlg = transfer == MediaFormat.COLOR_TRANSFER_HLG;
                boolean hdr10 = transfer == MediaFormat.COLOR_TRANSFER_ST2084 ||
                        format.containsKey(MediaFormat.KEY_HDR_STATIC_INFO);
                boolean hdr = dv || hlg || hdr10;
                return new VideoKind(hdr, hdr10 || (!hlg && dv), hlg, dv,
                        hasDolbyVisionDecoder(), mime);
            }
        } catch (Exception e) {
            setStatus("Video probe failed, using safe fallback: " + e.getMessage());
        } finally {
            extractor.release();
        }
        return new VideoKind(false, false, false, false, false, "unknown");
    }

    private boolean hasDolbyVisionDecoder() {
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
            if (info.isEncoder())
                continue;
            for (String type : info.getSupportedTypes()) {
                if ("video/dolby-vision".equalsIgnoreCase(type))
                    return true;
            }
        }
        return false;
    }

    private static final class VideoKind {
        final boolean hdr;
        final boolean hdr10;
        final boolean hlg;
        final boolean dolbyVision;
        final boolean dolbyVisionDecoder;
        final String mime;

        VideoKind(boolean hdr, boolean hdr10, boolean hlg, boolean dolbyVision,
                  boolean dolbyVisionDecoder, String mime) {
            this.hdr = hdr;
            this.hdr10 = hdr10;
            this.hlg = hlg;
            this.dolbyVision = dolbyVision;
            this.dolbyVisionDecoder = dolbyVisionDecoder;
            this.mime = mime;
        }

        String description() {
            if (dolbyVision)
                return "Dolby Vision";
            if (hlg)
                return "HLG";
            if (hdr10)
                return "HDR10/PQ";
            return "SDR";
        }
    }

    private void setStatus(String value) {
        if (status != null)
            status.setText(deviceReport() + "\n" + value);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surfaceReady = true;
        if (pendingFile != null)
            startOrLoad(pendingFile, pendingToneMapToSdr);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        if (mpv != 0) {
            nativeDestroy(mpv);
            mpv = 0;
        }
    }

    @Override
    protected void onDestroy() {
        if (mpv != 0) {
            nativeDestroy(mpv);
            mpv = 0;
        }
        super.onDestroy();
    }
}
