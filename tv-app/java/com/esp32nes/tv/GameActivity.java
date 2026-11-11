package com.esp32nes.tv;

import android.app.Activity;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.webkit.ConsoleMessage;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import android.widget.Toast;

import java.net.URLEncoder;

public class GameActivity extends Activity {
    private static final String TAG = "ESP32NES";
    private WebView web;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        immersive();

        final String ip = getIntent().getStringExtra("ip");
        final String rom = getIntent().getStringExtra("rom");
        if (ip == null || rom == null) {
            finish();
            return;
        }

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        web = new WebView(this);
        web.setBackgroundColor(Color.BLACK);
        web.setLayerType(View.LAYER_TYPE_HARDWARE, null);
        web.setLayoutParams(new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);
        s.setUseWideViewPort(true);
        s.setLoadWithOverviewMode(true);
        s.setSupportZoom(false);
        s.setBuiltInZoomControls(false);
        s.setDisplayZoomControls(false);
        if (Build.VERSION.SDK_INT >= 21) {
            s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        }
        web.setHorizontalScrollBarEnabled(false);
        web.setVerticalScrollBarEnabled(false);
        web.setOverScrollMode(View.OVER_SCROLL_NEVER);
        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onReceivedError(WebView view, int errorCode, String description,
                                        String failingUrl) {
                Log.e(TAG, "web error " + errorCode + " " + description + " " + failingUrl);
                Toast.makeText(GameActivity.this,
                        "页面加载失败：" + description, Toast.LENGTH_LONG).show();
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                Log.i(TAG, "page finished " + url);
            }
        });
        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onConsoleMessage(ConsoleMessage m) {
                Log.i(TAG, "js: " + m.message() + " @" + m.lineNumber());
                return true;
            }
        });
        root.addView(web);
        setContentView(root);

        String url;
        try {
            url = "http://" + ip + "/stream?rom=" + URLEncoder.encode(rom, "UTF-8")
                    + "&gate=1&kiosk=1";
        } catch (Exception e) {
            finish();
            return;
        }
        web.loadUrl(url);
        web.requestFocus(View.FOCUS_DOWN);
    }

    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    /* TV remotes often consume OK inside WebView. Intercept at the Activity
     * level and trigger the page's start gate on key-up; also pass the event
     * to WebView first so WebAudio sees a real user gesture. */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        final int code = event.getKeyCode();
        if (code == KeyEvent.KEYCODE_DPAD_CENTER || code == KeyEvent.KEYCODE_ENTER
                || code == KeyEvent.KEYCODE_NUMPAD_ENTER || code == KeyEvent.KEYCODE_BUTTON_A) {
            if (web != null) {
                web.dispatchKeyEvent(event);
                if (event.getAction() == KeyEvent.ACTION_UP) {
                    web.evaluateJavascript(
                            "(function(){var g=document.getElementById('gate');"
                                    + "if(g&&getComputedStyle(g).display!=='none'){"
                                    + "document.getElementById('gatebtn').click();"
                                    + "return 'started';}return 'idle';})()",
                            new ValueCallback<String>() {
                                @Override
                                public void onReceiveValue(String value) {
                                    Log.i(TAG, "gate result=" + value);
                                    if ("\"started\"".equals(value)) {
                                        Toast.makeText(GameActivity.this, "开始游戏…",
                                                Toast.LENGTH_SHORT).show();
                                    }
                                }
                            });
                }
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public void onBackPressed() {
        finish();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            immersive();
        }
    }

    @Override
    protected void onPause() {
        if (web != null) {
            web.onPause();
        }
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (web != null) {
            web.onResume();
        }
    }

    @Override
    protected void onDestroy() {
        if (web != null) {
            web.destroy();
            web = null;
        }
        super.onDestroy();
    }
}
