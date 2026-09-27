package org.mpv.androidtest;

import android.app.Activity;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.widget.FrameLayout;

import androidx.media3.common.Format;
import androidx.media3.common.MediaItem;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.exoplayer.DecoderCounters;
import androidx.media3.exoplayer.ExoPlayer;
import androidx.media3.exoplayer.analytics.AnalyticsListener;

import java.io.File;

public final class ExoActivity extends Activity {
    private static final String TAG = "exostat";
    private ExoPlayer player;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        SurfaceView surface = new SurfaceView(this);
        root.addView(surface);
        setContentView(root);

        player = new ExoPlayer.Builder(this).build();
        player.setVideoSurfaceView(surface);
        player.addAnalyticsListener(new AnalyticsListener() {
            @Override
            public void onVideoDecoderInitialized(EventTime t, String name, long a, long b) {
                Log.i(TAG, "decoder=" + name);
            }

            @Override
            public void onVideoInputFormatChanged(EventTime t, Format f,
                    androidx.media3.exoplayer.DecoderReuseEvaluation e) {
                Log.i(TAG, "format=" + Format.toLogString(f));
            }

            @Override
            public void onDroppedVideoFrames(EventTime t, int count, long elapsed) {
                Log.i(TAG, "dropped=" + count);
            }

            @Override
            public void onPlayerError(EventTime t, PlaybackException error) {
                Log.i(TAG, "error=" + error.getErrorCodeName() + " " + error.getMessage());
            }

            @Override
            public void onVideoDisabled(EventTime t, DecoderCounters c) {
                c.ensureUpdated();
                Log.i(TAG, "rendered=" + c.renderedOutputBufferCount + " dropped=" + c.droppedBufferCount);
            }
        });
        player.addListener(new Player.Listener() {
            @Override
            public void onPlaybackStateChanged(int s) {
                if (s == Player.STATE_ENDED)
                    Log.i(TAG, "ended");
            }
        });
        String path = getIntent().getStringExtra("path");
        player.setMediaItem(MediaItem.fromUri(Uri.fromFile(new File(path))));
        player.prepare();
        player.play();
    }

    @Override
    protected void onDestroy() {
        player.release();
        super.onDestroy();
    }
}
